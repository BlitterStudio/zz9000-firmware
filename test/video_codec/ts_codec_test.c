/*
 * MPEG Transport Stream through the MPEG-1/MPEG-2 video backend: the same
 * encode as a Program Stream and as a Transport Stream (also M2TS) must
 * decode to identical pictures and PCM with the same timeline.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "sdk_video_backend.h"
#include "sdk_video_yuy2.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ts_mpeg2_ps_fixture.inc"
#include "ts_mpeg2_ts_fixture.inc"
#include "ts_mpeg2_m2ts_fixture.inc"
#include "ts_mpeg1_ps_fixture.inc"
#include "ts_mpeg1_ts_fixture.inc"
#include "ts_mpeg2_2a_ps_fixture.inc"
#include "ts_mpeg2_2a_ts_fixture.inc"

#define PCM_RING_CAPACITY (128U * 1024U)
#define FRAMES 5U
#define FNV64_OFFSET UINT64_C(14695981039346656037)

#define CHECK(cond) \
	do { \
		if (!(cond)) { \
			fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, \
			        __LINE__, #cond); \
			return 1; \
		} \
	} while (0)

struct summary {
	uint32_t frames;
	uint32_t width;
	uint32_t height;
	uint32_t sample_rate;
	uint32_t media_flags;
	uint64_t pcm_produced;
	uint64_t video_hash;
	uint64_t pcm_hash;
	uint64_t first_video_pts;
	uint64_t last_video_pts;
	uint64_t first_audio_pts;
};

static uint8_t pcm_ring[PCM_RING_CAPACITY];

static uint64_t fnv(uint64_t hash, const uint8_t *bytes, uint32_t length)
{
	uint32_t i;

	for (i = 0U; i < length; i++) {
		hash ^= bytes[i];
		hash *= UINT64_C(1099511628211);
	}
	return hash;
}

static int accumulate(const struct SDKVideoDecodedFrame *frame,
                      struct summary *s)
{
	uint32_t row_bytes = sdk_video_yuy2_row_bytes(frame->width);
	uint32_t written;
	uint32_t row;
	uint8_t *yuy2 = (uint8_t *)malloc(row_bytes * frame->height);

	if (!yuy2 || !sdk_video_yuv420_to_yuy2(
	        yuy2, row_bytes, frame->width, frame->height, frame->y,
	        frame->y_pitch, frame->cb, frame->cr, frame->chroma_pitch,
	        &written)) {
		free(yuy2);
		return 1;
	}
	for (row = 0U; row < frame->height; row++)
		s->video_hash = fnv(s->video_hash, yuy2 + row * row_bytes,
		                    row_bytes);
	free(yuy2);
	if (s->frames == 0U)
		s->first_video_pts = frame->media_pts;
	s->last_video_pts = frame->media_pts;
	s->width = frame->width;
	s->height = frame->height;
	s->media_flags |= frame->media_flags;
	s->frames++;
	return 0;
}

static int decode(uint32_t codec, uint32_t container, const uint8_t *bytes,
                  uint32_t length, uint32_t chunk_size, uint32_t audio_codec,
                  struct summary *s)
{
	const struct SDKVideoDecoderOps *ops =
		sdk_video_backend_find(codec, container);
	struct SDKVideoMediaConfig config;
	struct SDKVideoMediaInfo media;
	struct SDKVideoDecodedFrame frame;
	uint32_t offset = 0U;
	uint32_t guard = 0U;
	int result = SDK_VIDEO_BACKEND_NEED_INPUT;
	void *decoder;

	memset(s, 0, sizeof(*s));
	s->video_hash = FNV64_OFFSET;
	s->pcm_hash = FNV64_OFFSET;
	memset(pcm_ring, 0xa5, sizeof(pcm_ring));
	if (!ops || !ops->configure_media || !ops->get_media_info ||
	    !ops->ack_media)
		return 1;
	decoder = ops->create(0U);
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
		uint32_t chunk = chunk_size < length - offset ? chunk_size
		                                              : length - offset;
		uint32_t accepted = 0U;

		if (!ops->write(decoder, bytes + offset, chunk, 0, &accepted) ||
		    accepted != chunk) {
			ops->destroy(decoder);
			return 4;
		}
		offset += accepted;
		for (;;) {
			result = ops->decode(decoder, &frame);
			if (result == SDK_VIDEO_BACKEND_FRAME) {
				if (accumulate(&frame, s) != 0) {
					ops->destroy(decoder);
					return 5;
				}
				continue;
			}
			if (result == SDK_VIDEO_BACKEND_PROGRESS)
				continue;
			if (result == SDK_VIDEO_BACKEND_ERROR) {
				ops->destroy(decoder);
				return 6;
			}
			break;
		}
	}
	{
		uint32_t accepted = 1U;

		if (!ops->write(decoder, 0, 0U, 1, &accepted) || accepted != 0U) {
			ops->destroy(decoder);
			return 7;
		}
	}
	while (result != SDK_VIDEO_BACKEND_DONE && guard++ < 1000U) {
		result = ops->decode(decoder, &frame);
		if (result == SDK_VIDEO_BACKEND_FRAME) {
			if (accumulate(&frame, s) != 0) {
				ops->destroy(decoder);
				return 8;
			}
		} else if (result == SDK_VIDEO_BACKEND_NEED_INPUT ||
		           result == SDK_VIDEO_BACKEND_ERROR) {
			ops->destroy(decoder);
			return 9;
		}
	}
	if (result != SDK_VIDEO_BACKEND_DONE ||
	    !ops->get_media_info(decoder, &media)) {
		ops->destroy(decoder);
		return 10;
	}
	s->sample_rate = media.sample_rate;
	s->media_flags |= media.flags;
	s->pcm_produced = media.pcm_produced;
	s->first_audio_pts = media.first_audio_pts;
	if (audio_codec == SDK_VIDEO_MEDIA_AUDIO_MP2)
		s->pcm_hash = fnv(s->pcm_hash, pcm_ring,
		                  (uint32_t)media.pcm_produced);
	ops->destroy(decoder);
	return 0;
}

static int same(const struct summary *a, const struct summary *b,
                const char *what)
{
	if (memcmp(a, b, sizeof(*a)) == 0)
		return 1;
	fprintf(stderr,
	        "%s: summaries differ: frames %u/%u pcm %llu/%llu video %llx/%llx "
	        "pcm %llx/%llx pts %llu/%llu audio pts %llu/%llu flags %x/%x\n",
	        what, a->frames, b->frames, (unsigned long long)a->pcm_produced,
	        (unsigned long long)b->pcm_produced,
	        (unsigned long long)a->video_hash,
	        (unsigned long long)b->video_hash,
	        (unsigned long long)a->pcm_hash, (unsigned long long)b->pcm_hash,
	        (unsigned long long)a->first_video_pts,
	        (unsigned long long)b->first_video_pts,
	        (unsigned long long)a->first_audio_pts,
	        (unsigned long long)b->first_audio_pts, a->media_flags,
	        b->media_flags);
	return 0;
}

static const uint32_t chunks[] = {188U, 192U, 1000U, 4096U, 65536U};

/* A Program Stream and its Transport Stream twin decode alike. */
static int test_decode_matches(uint32_t codec, const uint8_t *ps,
                               uint32_t ps_len, const uint8_t *ts,
                               uint32_t ts_len, const char *what)
{
	struct summary ps_sum;
	struct summary ts_sum;
	uint32_t i;
	int r;

	r = decode(codec, SDK_VIDEO_CONTAINER_MPEG_PS, ps, ps_len, 2048U,
	           SDK_VIDEO_MEDIA_AUDIO_MP2, &ps_sum);
	if (r != 0) {
		fprintf(stderr, "%s: Program Stream decode failed (%d)\n", what, r);
		return 1;
	}
	CHECK(ps_sum.frames == FRAMES);
	CHECK(ps_sum.pcm_produced != 0U);
	for (i = 0U; i < sizeof(chunks) / sizeof(chunks[0]); i++) {
		r = decode(codec, SDK_VIDEO_CONTAINER_MPEG_TS, ts, ts_len,
		           chunks[i], SDK_VIDEO_MEDIA_AUDIO_MP2, &ts_sum);
		if (r != 0) {
			fprintf(stderr, "%s: Transport Stream decode failed (%d), "
			        "chunk %u\n", what, r, chunks[i]);
			return 1;
		}
		if (!same(&ps_sum, &ts_sum, what)) {
			fprintf(stderr, "  chunk %u\n", chunks[i]);
			return 1;
		}
	}
	/* Without audio the picture stream alone plays and ends. */
	r = decode(codec, SDK_VIDEO_CONTAINER_MPEG_TS, ts, ts_len, 4096U,
	           SDK_VIDEO_MEDIA_AUDIO_NONE, &ts_sum);
	CHECK(r == 0);
	CHECK(ts_sum.frames == FRAMES);
	CHECK(ts_sum.video_hash == ps_sum.video_hash);
	CHECK(ts_sum.pcm_produced == 0U);
	return 0;
}

static int test_pictures(void)
{
	CHECK(test_decode_matches(SDK_VIDEO_CODEC_MPEG2,
	                          zz9k_ts_mpeg2_ps_fixture,
	                          zz9k_ts_mpeg2_ps_fixture_len,
	                          zz9k_ts_mpeg2_ts_fixture,
	                          zz9k_ts_mpeg2_ts_fixture_len,
	                          "MPEG-2 TS") == 0);
	CHECK(test_decode_matches(SDK_VIDEO_CODEC_MPEG2,
	                          zz9k_ts_mpeg2_ps_fixture,
	                          zz9k_ts_mpeg2_ps_fixture_len,
	                          zz9k_ts_mpeg2_m2ts_fixture,
	                          zz9k_ts_mpeg2_m2ts_fixture_len,
	                          "MPEG-2 M2TS") == 0);
	CHECK(test_decode_matches(SDK_VIDEO_CODEC_MPEG1,
	                          zz9k_ts_mpeg1_ps_fixture,
	                          zz9k_ts_mpeg1_ps_fixture_len,
	                          zz9k_ts_mpeg1_ts_fixture,
	                          zz9k_ts_mpeg1_ts_fixture_len,
	                          "MPEG-1 TS") == 0);
	CHECK(test_decode_matches(SDK_VIDEO_CODEC_MPEG2,
	                          zz9k_ts_mpeg2_2a_ps_fixture,
	                          zz9k_ts_mpeg2_2a_ps_fixture_len,
	                          zz9k_ts_mpeg2_2a_ts_fixture,
	                          zz9k_ts_mpeg2_2a_ts_fixture_len,
	                          "two audio tracks") == 0);
	return 0;
}

/* The Transport Stream backends exist for MPEG-1 and MPEG-2 only, and carry
 * MPEG audio: AC-3 and LPCM belong to the MPEG-2 Program Stream. */
static int test_registry(void)
{
	const struct SDKVideoDecoderOps *ts1 = sdk_video_backend_find(
		SDK_VIDEO_CODEC_MPEG1, SDK_VIDEO_CONTAINER_MPEG_TS);
	const struct SDKVideoDecoderOps *ts2 = sdk_video_backend_find(
		SDK_VIDEO_CODEC_MPEG2, SDK_VIDEO_CONTAINER_MPEG_TS);
	const struct SDKVideoDecoderOps *ps2 = sdk_video_backend_find(
		SDK_VIDEO_CODEC_MPEG2, SDK_VIDEO_CONTAINER_MPEG_PS);

	CHECK(SDK_VIDEO_CONTAINER_MPEG_TS == 3U);
	CHECK(ts1 && ts2 && ps2 && ts1 != ts2 && ts2 != ps2);
	CHECK(ts1->container == SDK_VIDEO_CONTAINER_MPEG_TS);
	CHECK(ts2->container == SDK_VIDEO_CONTAINER_MPEG_TS);
	CHECK(ts1->codec == SDK_VIDEO_CODEC_MPEG1);
	CHECK(ts2->codec == SDK_VIDEO_CODEC_MPEG2);
	CHECK(ts1->audio_ok(SDK_VIDEO_MEDIA_AUDIO_NONE));
	CHECK(ts1->audio_ok(SDK_VIDEO_MEDIA_AUDIO_MP2));
	CHECK(ts2->audio_ok(SDK_VIDEO_MEDIA_AUDIO_NONE));
	CHECK(ts2->audio_ok(SDK_VIDEO_MEDIA_AUDIO_MP2));
	CHECK(!ts2->audio_ok(SDK_VIDEO_MEDIA_AUDIO_AC3));
	CHECK(!ts2->audio_ok(SDK_VIDEO_MEDIA_AUDIO_LPCM));
	CHECK(!ts1->audio_ok(SDK_VIDEO_MEDIA_AUDIO_AC3));
	CHECK(ps2->audio_ok(SDK_VIDEO_MEDIA_AUDIO_AC3));
	CHECK(sdk_video_backend_find(SDK_VIDEO_CODEC_VP8,
	                             SDK_VIDEO_CONTAINER_MPEG_TS) == 0);
	CHECK(sdk_video_backend_find(SDK_VIDEO_CODEC_VP9,
	                             SDK_VIDEO_CONTAINER_MPEG_TS) == 0);
	/* A session configured for AC-3 is refused at configure time too. */
	{
		void *decoder = ts2->create(0U);
		struct SDKVideoMediaConfig config;

		CHECK(decoder != 0);
		memset(&config, 0, sizeof(config));
		config.audio_codec = SDK_VIDEO_MEDIA_AUDIO_AC3;
		config.pcm_ring = pcm_ring;
		config.pcm_ring_capacity = sizeof(pcm_ring);
		config.pcm_low_water_bytes = 4608U;
		config.pcm_high_water_bytes = sizeof(pcm_ring);
		CHECK(!ts2->configure_media(decoder, &config));
		ts2->destroy(decoder);
	}
	return 0;
}

/* Damage in the transport layer costs pictures, not the session: a packet
 * lost from the middle of the stream still ends with DONE. */
static int test_lost_packet(void)
{
	struct summary s;
	uint32_t length = zz9k_ts_mpeg2_ts_fixture_len - 188U;
	uint8_t *damaged = (uint8_t *)malloc(zz9k_ts_mpeg2_ts_fixture_len);
	uint32_t cut = 188U * 20U;
	int r;

	memcpy(damaged, zz9k_ts_mpeg2_ts_fixture, cut);
	memcpy(damaged + cut, zz9k_ts_mpeg2_ts_fixture + cut + 188U,
	       zz9k_ts_mpeg2_ts_fixture_len - cut - 188U);
	r = decode(SDK_VIDEO_CODEC_MPEG2, SDK_VIDEO_CONTAINER_MPEG_TS, damaged,
	           length, 4096U, SDK_VIDEO_MEDIA_AUDIO_MP2, &s);
	free(damaged);
	CHECK(r == 0);
	CHECK(s.frames >= 3U && s.frames <= FRAMES);
	return 0;
}

int main(void)
{
	struct {
		const char *name;
		int (*fn)(void);
	} tests[] = {
		{"Transport Stream decodes like its Program Stream", test_pictures},
		{"Transport Stream backend registry", test_registry},
		{"lost packet", test_lost_packet},
	};
	unsigned i;
	int failed = 0;

	for (i = 0U; i < sizeof(tests) / sizeof(tests[0]); i++) {
		int r = tests[i].fn();

		printf("%s: %s\n", r == 0 ? "ok" : "FAIL", tests[i].name);
		if (r != 0)
			failed++;
	}
	return failed != 0;
}
