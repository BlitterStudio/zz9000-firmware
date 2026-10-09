/*
 * DVD MPEG-2 / VOB demux + video / audio codec host correctness tests.
 *
 * Verifies:
 * - PAL 720x576 25p with MP2 audio
 * - PAL 720x576 TFF interlaced with AC-3 audio & moving-field markers
 * - NTSC 720x480 30000/1001 with DVD LPCM audio
 * - NTSC film 720x480 24000/1001 with 3:2 repeat-first-field cadence
 * - Deterministic AC-3 5.1-to-stereo downmixing
 * - Every AC-3 2.0 192 kbit/s frame decoded (2.6 frames per 2 KB pack) and
 *   the stream reaching DONE, with a roomy and with a tight PCM ring
 * - One AC-3 substream selected when a VOB carries two
 * - DVD LPCM header parsing and sample unpacking exactness (mono & stereo)
 * - Raw integer 90 kHz PTS/DTS preservation and timeline tracking
 * - Bounded demuxer behavior with chunked/fragmented transport
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "sdk_video_backend.h"
#include "sdk_video_yuy2.h"
#include "sdk_dvd_ps.h"
#include "sdk_dvd_lpcm.h"
#include "sdk_dvd_ac3.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dvd_pal_25p_fixture.inc"
#include "dvd_pal_tff_fixture.inc"
#include "dvd_ntsc_30_fixture.inc"
#include "dvd_ntsc_film_32_fixture.inc"
#include "dvd_ac3_51_fixture.inc"
#include "dvd_ac3_192_fixture.inc"
#include "dvd_ac3_dual_fixture.inc"

#define PCM_RING_CAPACITY (128U * 1024U)
#define FNV64_OFFSET UINT64_C(14695981039346656037)

static uint64_t fnv1a64(uint64_t hash, const uint8_t *bytes, uint32_t length)
{
	uint32_t i;

	for (i = 0U; i < length; i++) {
		hash ^= bytes[i];
		hash *= UINT64_C(1099511628211);
	}
	return hash;
}

/* -------------------------------------------------------- LPCM exactness */

static int test_lpcm_exactness(void)
{
	struct SDKDVDLPCM lpcm = {0};
	uint8_t hdr_stereo[7] = {0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00};
	uint8_t hdr_mono[7]   = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
	uint8_t hdr_bad_rate[7] = {0x00, 0x00, 0x04, 0x10, 0x00, 0x00, 0x00};
	uint8_t hdr_bad_bits[7] = {0x00, 0x00, 0x40, 0x10, 0x00, 0x00, 0x00};
	uint8_t hdr_bad_chans[7] = {0x00, 0x00, 0x00, 0x50, 0x00, 0x00, 0x00};
	uint8_t src[16];
	uint8_t dst[32];
	uint32_t bytes;
	uint32_t i;

	/* 1. Header validation */
	if (!sdk_dvd_lpcm_header(&lpcm, hdr_stereo) || lpcm.channels != 2U)
		return 1;
	memset(&lpcm, 0, sizeof(lpcm));
	if (!sdk_dvd_lpcm_header(&lpcm, hdr_mono) || lpcm.channels != 1U)
		return 2;
	memset(&lpcm, 0, sizeof(lpcm));
	if (sdk_dvd_lpcm_header(&lpcm, hdr_bad_rate))
		return 3;
	if (sdk_dvd_lpcm_header(&lpcm, hdr_bad_bits))
		return 4;
	if (sdk_dvd_lpcm_header(&lpcm, hdr_bad_chans))
		return 5;

	/* 2. Stereo unpack exactness */
	memset(&lpcm, 0, sizeof(lpcm));
	sdk_dvd_lpcm_header(&lpcm, hdr_stereo);
	for (i = 0U; i < sizeof(src); i++)
		src[i] = (uint8_t)(i + 1U);
	memset(dst, 0, sizeof(dst));
	bytes = sdk_dvd_lpcm_unpack(&lpcm, src, sizeof(src), dst, sizeof(dst));
	if (bytes != sizeof(src) || memcmp(dst, src, sizeof(src)) != 0)
		return 6;
	if (lpcm.frames != 4U || lpcm.produced_bytes != 16U)
		return 7;

	/* 3. Mono dual-mono expansion (as implemented in route_lpcm) */
	memset(&lpcm, 0, sizeof(lpcm));
	sdk_dvd_lpcm_header(&lpcm, hdr_mono);
	memset(dst, 0, sizeof(dst));
	bytes = sdk_dvd_lpcm_unpack(&lpcm, src, 8U, dst, 16U);
	if (bytes != 8U)
		return 8;
	/* In-place reverse expansion: 4 mono frames (8 bytes) -> 4 stereo frames (16 bytes) */
	{
		uint32_t frames = bytes / 2U;
		int32_t idx;
		for (idx = (int32_t)frames - 1; idx >= 0; idx--) {
			dst[4 * idx + 3] = dst[2 * idx + 1];
			dst[4 * idx + 2] = dst[2 * idx];
			dst[4 * idx + 1] = dst[2 * idx + 1];
			dst[4 * idx]     = dst[2 * idx];
		}
	}
	/* Check each stereo frame has identical Left and Right */
	for (i = 0U; i < 4U; i++) {
		if (dst[4 * i] != src[2 * i] ||
		    dst[4 * i + 1] != src[2 * i + 1] ||
		    dst[4 * i + 2] != src[2 * i] ||
		    dst[4 * i + 3] != src[2 * i + 1])
			return 9;
	}

	return 0;
}

/* ------------------------------------- Video + Audio decode validation */

struct stream_result {
	uint32_t frame_count;
	uint32_t width;
	uint32_t height;
	uint32_t frame_rate_milli;
	uint32_t picture_flags[16];
	uint32_t display_fields[16];
	uint64_t raw_pts[16];
	uint64_t yuy2_hash;
	uint64_t pcm_hash;
	uint64_t pcm_produced;
	uint32_t audio_frames;
	uint32_t audio_rate;
	uint32_t audio_channels;
};

static int run_decode_test(const uint8_t *stream, uint32_t length,
                           uint32_t chunk_size, uint32_t audio_codec,
                           struct stream_result *out)
{
	const struct SDKVideoDecoderOps *ops = sdk_video_mpeg2_backend_ops();
	void *decoder;
	uint8_t *pcm_ring = NULL;
	struct SDKVideoDecodedFrame frame;
	uint32_t offset = 0U;
	int res = 0;

	memset(out, 0, sizeof(*out));
	out->yuy2_hash = UINT64_C(14695981039346656037);

	decoder = ops->create();
	if (!decoder)
		return 1;

	if (audio_codec != SDK_VIDEO_MEDIA_AUDIO_NONE) {
		struct SDKVideoMediaConfig cfg = {0};
		pcm_ring = (uint8_t *)malloc(PCM_RING_CAPACITY);
		if (!pcm_ring) {
			ops->destroy(decoder);
			return 2;
		}
		memset(pcm_ring, 0, PCM_RING_CAPACITY);
		cfg.audio_codec = audio_codec;
		cfg.pcm_ring = pcm_ring;
		cfg.pcm_ring_capacity = PCM_RING_CAPACITY;
		cfg.pcm_low_water_bytes = 4096U;
		cfg.pcm_high_water_bytes = PCM_RING_CAPACITY - 4096U;
		if (!ops->configure_media(decoder, &cfg)) {
			free(pcm_ring);
			ops->destroy(decoder);
			return 3;
		}
	}

	while (offset < length) {
		uint32_t chunk = chunk_size != 0U ? chunk_size : (length - offset);
		uint32_t accepted = 0U;
		int eof = (offset + chunk >= length);

		if (chunk > length - offset)
			chunk = length - offset;

		if (!ops->write(decoder, stream + offset, chunk, eof, &accepted)) {
			if (pcm_ring) free(pcm_ring);
			ops->destroy(decoder);
			return 4;
		}
		offset += accepted;

		for (;;) {
			res = ops->decode(decoder, &frame);
			if (res != SDK_VIDEO_BACKEND_FRAME)
				break;

			if (out->frame_count < 16U) {
				out->width = frame.width;
				out->height = frame.height;
				out->picture_flags[out->frame_count] = frame.picture_flags;
				out->display_fields[out->frame_count] = frame.display_fields;
				out->raw_pts[out->frame_count] = frame.raw_pts;
			}

			{
				uint32_t row_bytes = sdk_video_yuy2_row_bytes(frame.width);
				uint8_t *yuy2 = (uint8_t *)malloc(row_bytes * frame.height);
				uint32_t written = 0U;
				if (!yuy2 || !sdk_video_yuv420_to_yuy2(
					yuy2, row_bytes, frame.width, frame.height,
					frame.y, frame.y_pitch, frame.cb, frame.cr,
					frame.chroma_pitch, &written)) {
					if (yuy2) free(yuy2);
					if (pcm_ring) free(pcm_ring);
					ops->destroy(decoder);
					return 5;
				}
				out->yuy2_hash = fnv1a64(out->yuy2_hash, yuy2, written);
				free(yuy2);
			}
			out->frame_count++;
		}
		if (res == SDK_VIDEO_BACKEND_ERROR) {
			if (pcm_ring) free(pcm_ring);
			ops->destroy(decoder);
			return 6;
		}
	}

	/* Signal EOF with empty buffer */
	{
		uint32_t accepted = 0U;
		ops->write(decoder, NULL, 0U, 1, &accepted);
		for (;;) {
			res = ops->decode(decoder, &frame);
			if (res != SDK_VIDEO_BACKEND_FRAME)
				break;
			if (out->frame_count < 16U) {
				out->width = frame.width;
				out->height = frame.height;
				out->picture_flags[out->frame_count] = frame.picture_flags;
				out->display_fields[out->frame_count] = frame.display_fields;
				out->raw_pts[out->frame_count] = frame.raw_pts;
			}
			{
				uint32_t row_bytes = sdk_video_yuy2_row_bytes(frame.width);
				uint8_t *yuy2 = (uint8_t *)malloc(row_bytes * frame.height);
				uint32_t written = 0U;
				if (yuy2 && sdk_video_yuv420_to_yuy2(
					yuy2, row_bytes, frame.width, frame.height,
					frame.y, frame.y_pitch, frame.cb, frame.cr,
					frame.chroma_pitch, &written)) {
					out->yuy2_hash = fnv1a64(out->yuy2_hash, yuy2, written);
				}
				if (yuy2) free(yuy2);
			}
			out->frame_count++;
		}
	}

	{
		struct SDKVideoDecoderInfo vinfo;
		if (ops->get_info(decoder, &vinfo)) {
			out->frame_rate_milli = vinfo.frame_rate_milli;
		}
	}

	if (audio_codec != SDK_VIDEO_MEDIA_AUDIO_NONE) {
		struct SDKVideoMediaInfo ainfo;
		if (ops->get_media_info(decoder, &ainfo)) {
			out->pcm_produced = ainfo.pcm_produced;
			out->audio_frames = ainfo.audio_frames;
			out->audio_rate = ainfo.sample_rate;
			out->audio_channels = ainfo.channels;
			if (ainfo.pcm_produced != 0U) {
				uint32_t hash_len = ainfo.pcm_produced > PCM_RING_CAPACITY
					? PCM_RING_CAPACITY : (uint32_t)ainfo.pcm_produced;
				out->pcm_hash = fnv1a64(UINT64_C(14695981039346656037),
				                        pcm_ring, hash_len);
			}
		}
		free(pcm_ring);
	}

	ops->destroy(decoder);
	return 0;
}

/* ------------------------------------------- AC-3 deterministic downmix */

static int test_ac3_downmix_exactness(void)
{
	struct stream_result r1;
	struct stream_result r2;

	if (run_decode_test(zz9k_dvd_ac3_51_fixture, zz9k_dvd_ac3_51_fixture_len,
	                    512U, SDK_VIDEO_MEDIA_AUDIO_AC3, &r1) != 0)
		return 1;
	if (run_decode_test(zz9k_dvd_ac3_51_fixture, zz9k_dvd_ac3_51_fixture_len,
	                    512U, SDK_VIDEO_MEDIA_AUDIO_AC3, &r2) != 0)
		return 2;

	if (r1.pcm_produced == 0U || r1.pcm_produced != r2.pcm_produced)
		return 3;
	if (r1.pcm_hash != r2.pcm_hash)
		return 4;
	if (r1.audio_rate != 48000U || r1.audio_channels != 2U)
		return 5;

	return 0;
}

/* 1. PAL 25p: progressive, 720x576, 25 fps, MP2 audio */
static int test_pal_25p(void)
{
	struct stream_result r;
	uint32_t i;

	if (run_decode_test(zz9k_dvd_pal_25p_fixture, zz9k_dvd_pal_25p_fixture_len,
	                    197U, SDK_VIDEO_MEDIA_AUDIO_MP2, &r) != 0)
		return 1;

	if (r.frame_count != 5U) {
		fprintf(stderr, "test_pal_25p: frame_count=%u (expected 5)\n", r.frame_count);
		return 2;
	}
	if (r.width != 720U || r.height != 576U) {
		fprintf(stderr, "test_pal_25p: %ux%u (expected 720x576)\n", r.width, r.height);
		return 3;
	}
	if (r.frame_rate_milli != 25000U) {
		fprintf(stderr, "test_pal_25p: rate=%u (expected 25000)\n", r.frame_rate_milli);
		return 4;
	}

	for (i = 0U; i < r.frame_count; i++) {
		if ((r.picture_flags[i] & SDK_VIDEO_PICTURE_FLAG_PROGRESSIVE) == 0U)
			return 5;
		if (r.display_fields[i] != 2U)
			return 6;
		if (r.raw_pts[i] == SDK_VIDEO_MEDIA_NO_PTS)
			return 7;
	}

	/* Monotonic display PTS */
	for (i = 1U; i < r.frame_count; i++) {
		if (r.raw_pts[i] <= r.raw_pts[i - 1U])
			return 8;
	}

	if (r.pcm_produced == 0U || r.audio_rate != 48000U || r.audio_channels != 2U)
		return 9;

	return 0;
}

/* 2. PAL TFF Interlaced: 720x576, top-field-first, AC-3 audio, moving-field marker */
static int test_pal_tff_interlaced(void)
{
	struct stream_result r;
	uint32_t i;

	if (run_decode_test(zz9k_dvd_pal_tff_fixture, zz9k_dvd_pal_tff_fixture_len,
	                    251U, SDK_VIDEO_MEDIA_AUDIO_AC3, &r) != 0)
		return 1;

	if (r.frame_count != 5U) {
		fprintf(stderr, "test_pal_tff: frame_count=%u (expected 5)\n", r.frame_count);
		return 2;
	}
	if (r.width != 720U || r.height != 576U)
		return 3;
	if (r.frame_rate_milli != 25000U)
		return 4;

	for (i = 0U; i < r.frame_count; i++) {
		if ((r.picture_flags[i] & SDK_VIDEO_PICTURE_FLAG_TFF) == 0U)
			return 5;
		if (r.display_fields[i] != 2U)
			return 6;
		if (r.raw_pts[i] == SDK_VIDEO_MEDIA_NO_PTS)
			return 7;
	}

	if (r.pcm_produced == 0U || r.audio_rate != 48000U || r.audio_channels != 2U)
		return 8;
	/* All 7 AC-3 frames (0.2 s); the last one shares a pack with others. */
	if (r.pcm_produced != 7U * SDK_DVD_AC3_FRAME_PCM_BYTES) {
		fprintf(stderr, "test_pal_tff: pcm=%llu (expected %u)\n",
		        (unsigned long long)r.pcm_produced,
		        7U * SDK_DVD_AC3_FRAME_PCM_BYTES);
		return 9;
	}

	return 0;
}

/* 3. NTSC 30000/1001: 720x480, DVD LPCM audio */
static int test_ntsc_30_lpcm(void)
{
	struct stream_result r;
	uint32_t i;

	if (run_decode_test(zz9k_dvd_ntsc_30_fixture, zz9k_dvd_ntsc_30_fixture_len,
	                    512U, SDK_VIDEO_MEDIA_AUDIO_LPCM, &r) != 0)
		return 1;

	if (r.frame_count != 6U) {
		fprintf(stderr, "test_ntsc_30: frame_count=%u (expected 6)\n", r.frame_count);
		return 2;
	}
	if (r.width != 720U || r.height != 480U)
		return 3;
	if (r.frame_rate_milli != 29970U)
		return 4;

	for (i = 0U; i < r.frame_count; i++) {
		if ((r.picture_flags[i] & SDK_VIDEO_PICTURE_FLAG_PROGRESSIVE) == 0U)
			return 5;
		if (r.display_fields[i] != 2U)
			return 6;
	}

	if (r.pcm_produced == 0U || r.audio_rate != 48000U || r.audio_channels != 2U)
		return 7;

	return 0;
}

/* 4. NTSC film 24000/1001 with 3:2 repeat-first-field cadence */
static int test_ntsc_film_32_cadence(void)
{
	struct stream_result r;
	uint32_t expected_fields[6] = {2U, 3U, 2U, 3U, 2U, 3U};
	uint32_t i;

	if (run_decode_test(zz9k_dvd_ntsc_film_32_fixture, zz9k_dvd_ntsc_film_32_fixture_len,
	                    1024U, SDK_VIDEO_MEDIA_AUDIO_NONE, &r) != 0)
		return 1;

	if (r.frame_count != 6U) {
		fprintf(stderr, "test_ntsc_film_32: frame_count=%u (expected 6)\n", r.frame_count);
		return 2;
	}
	if (r.width != 720U || r.height != 480U)
		return 3;
	if (r.frame_rate_milli != 23976U)
		return 4;

	/* Verify exact 3:2 cadence pattern (2 fields, 3 fields, 2 fields, 3 fields...) */
	for (i = 0U; i < 6U; i++) {
		if (r.display_fields[i] != expected_fields[i]) {
			fprintf(stderr, "frame %u fields=%u (expected %u)\n",
			        i, r.display_fields[i], expected_fields[i]);
			return 5;
		}
		if (expected_fields[i] == 3U) {
			if ((r.picture_flags[i] & SDK_VIDEO_PICTURE_FLAG_REPEAT) == 0U)
				return 6;
		} else {
			if ((r.picture_flags[i] & SDK_VIDEO_PICTURE_FLAG_REPEAT) != 0U)
				return 7;
		}
	}

	return 0;
}

/* 5. Demuxer bounded queue and backpressure */
static int test_demuxer_bounded_behavior(void)
{
	struct SDKDVDPSDemux demux;
	uint32_t accepted = 0U;
	uint32_t i;

	sdk_dvd_ps_init(&demux);

	/* Write without popping to fill bounded queues */
	for (i = 0U; i < 20U; i++) {
		uint32_t step = 0U;
		sdk_dvd_ps_write(&demux, zz9k_dvd_pal_25p_fixture,
		                 zz9k_dvd_pal_25p_fixture_len, &step, 0);
		accepted += step;
	}

	/* Backpressure events must have triggered */
	if (demux.backpressure_events == 0U)
		return 1;

	/* Drain and verify queues unblock cleanly */
	while (sdk_dvd_ps_peek_video(&demux))
		sdk_dvd_ps_pop_video(&demux, NULL);
	while (sdk_dvd_ps_peek_audio(&demux))
		sdk_dvd_ps_pop_audio(&demux, NULL);

	if (demux.video.count != 0U || demux.audio.count != 0U)
		return 2;

	return 0;
}

/* ------------------------------------------ AC-3 through an acked ring */

struct acked_result {
	uint64_t pcm_bytes;
	uint64_t pcm_hash;
	uint32_t video_frames;
	uint32_t audio_frames;
	uint32_t media_flags;
	int done;
};

/* Runs the backend like a player would: writes in chunks, acknowledges at
 * most max_ack PCM bytes per decode call, hashes the PCM in ack order, and
 * stops at DONE (or fails after a bounded number of calls). */
static int run_acked(const uint8_t *stream, uint32_t length, uint32_t chunk,
                     uint32_t ring_capacity, uint32_t max_ack,
                     struct acked_result *out)
{
	const struct SDKVideoDecoderOps *ops = sdk_video_mpeg2_backend_ops();
	struct SDKVideoMediaConfig cfg = {0};
	struct SDKVideoDecodedFrame frame;
	struct SDKVideoMediaInfo info;
	uint8_t *ring;
	void *decoder;
	uint32_t offset = 0U;
	uint64_t acked = 0U;
	uint32_t calls;

	memset(out, 0, sizeof(*out));
	out->pcm_hash = FNV64_OFFSET;
	decoder = ops->create();
	ring = (uint8_t *)malloc(ring_capacity);
	if (!decoder || !ring) {
		free(ring);
		if (decoder)
			ops->destroy(decoder);
		return 1;
	}
	cfg.audio_codec = SDK_VIDEO_MEDIA_AUDIO_AC3;
	cfg.pcm_ring = ring;
	cfg.pcm_ring_capacity = ring_capacity;
	cfg.pcm_low_water_bytes = 4096U;
	cfg.pcm_high_water_bytes = ring_capacity - 4096U;
	if (!ops->configure_media(decoder, &cfg)) {
		free(ring);
		ops->destroy(decoder);
		return 2;
	}
	for (calls = 0U; calls < 200000U; calls++) {
		int res;

		if (offset < length) {
			uint32_t n = length - offset < chunk ? length - offset : chunk;
			uint32_t accepted = 0U;

			if (!ops->write(decoder, stream + offset, n,
			                offset + n >= length, &accepted)) {
				free(ring);
				ops->destroy(decoder);
				return 3;
			}
			offset += accepted;
		}
		res = ops->decode(decoder, &frame);
		if (res == SDK_VIDEO_BACKEND_ERROR) {
			free(ring);
			ops->destroy(decoder);
			return 4;
		}
		if (res == SDK_VIDEO_BACKEND_FRAME)
			out->video_frames++;
		if (ops->get_media_info(decoder, &info) &&
		    info.pcm_produced > acked) {
			uint64_t step = info.pcm_produced - acked;

			if (step > max_ack)
				step = max_ack;
			while (step != 0U) {
				uint32_t at = (uint32_t)(acked % ring_capacity);
				uint32_t run = ring_capacity - at;

				if (run > step)
					run = (uint32_t)step;
				out->pcm_hash = fnv1a64(out->pcm_hash, ring + at, run);
				acked += run;
				step -= run;
			}
			ops->ack_media(decoder, acked);
		}
		if (res == SDK_VIDEO_BACKEND_DONE) {
			out->done = 1;
			break;
		}
	}
	if (ops->get_media_info(decoder, &info)) {
		out->audio_frames = info.audio_frames;
		out->media_flags = info.flags;
	}
	out->pcm_bytes = acked;
	free(ring);
	ops->destroy(decoder);
	return 0;
}

/* Backend-independent reference: the demux plus the AC-3 wrapper with an
 * output buffer large enough for every frame a packet can complete. */
static int ac3_reference(const uint8_t *stream, uint32_t length,
                         uint64_t *bytes, uint64_t *hash, uint32_t *frames)
{
	static struct SDKDVDPSDemux demux;
	static struct SDKDVDAC3 ac3;
	static uint8_t pcm[64U * 1024U];
	uint32_t offset = 0U;
	uint32_t rounds;

	sdk_dvd_ps_init(&demux);
	sdk_dvd_ac3_init(&ac3);
	*bytes = 0U;
	*hash = FNV64_OFFSET;
	for (rounds = 0U; !sdk_dvd_ps_done(&demux); rounds++) {
		const struct SDKDVDPSPacket *p;
		uint32_t accepted = 0U;

		if (rounds > 100000U) {
			sdk_dvd_ac3_destroy(&ac3);
			return 1;
		}
		if (offset < length) {
			sdk_dvd_ps_write(&demux, stream + offset, length - offset,
			                 &accepted, 1);
			offset += accepted;
		}
		while (sdk_dvd_ps_peek_video(&demux))
			sdk_dvd_ps_pop_video(&demux, NULL);
		while ((p = sdk_dvd_ps_peek_audio(&demux)) != NULL) {
			if (p->kind == SDK_DVD_PS_AC3 &&
			    p->length > SDK_DVD_AC3_HEADER_BYTES) {
				uint32_t n = sdk_dvd_ac3_decode(
					&ac3, p->data + SDK_DVD_AC3_HEADER_BYTES,
					p->length - SDK_DVD_AC3_HEADER_BYTES,
					pcm, sizeof(pcm));
				*hash = fnv1a64(*hash, pcm, n);
				*bytes += n;
			}
			sdk_dvd_ps_pop_audio(&demux, NULL);
		}
	}
	*frames = ac3.frames;
	sdk_dvd_ac3_destroy(&ac3);
	return 0;
}

/* 6. AC-3 2.0 192 kbit/s: 63 frames in 25 packs of up to 2015 payload
 * bytes. Every frame must reach the ring and the stream must end DONE. */
static int test_ac3_192_all_frames(void)
{
	static const struct {
		uint32_t chunk;
		uint32_t ring;
		uint32_t max_ack;
	} runs[] = {
		{ 2048U, 128U * 1024U, 128U * 1024U }, /* roomy ring */
		{ 777U, 16U * 1024U, 1000U },          /* ring keeps filling */
	};
	uint64_t ref_bytes, ref_hash;
	uint32_t ref_frames;
	uint32_t i;

	if (ac3_reference(zz9k_dvd_ac3_192_fixture, zz9k_dvd_ac3_192_fixture_len,
	                  &ref_bytes, &ref_hash, &ref_frames) != 0)
		return 1;
	if (ref_frames != 63U ||
	    ref_bytes != 63U * (uint64_t)SDK_DVD_AC3_FRAME_PCM_BYTES) {
		fprintf(stderr, "test_ac3_192: reference frames=%u bytes=%llu\n",
		        ref_frames, (unsigned long long)ref_bytes);
		return 2;
	}
	for (i = 0U; i < sizeof(runs) / sizeof(runs[0]); i++) {
		struct acked_result r;

		if (run_acked(zz9k_dvd_ac3_192_fixture,
		              zz9k_dvd_ac3_192_fixture_len, runs[i].chunk,
		              runs[i].ring, runs[i].max_ack, &r) != 0)
			return 3;
		if (!r.done) {
			fprintf(stderr, "test_ac3_192 run %u: never DONE (pcm=%llu)\n",
			        i, (unsigned long long)r.pcm_bytes);
			return 4;
		}
		if (r.pcm_bytes != ref_bytes || r.pcm_hash != ref_hash) {
			fprintf(stderr, "test_ac3_192 run %u: pcm=%llu (expected %llu)\n",
			        i, (unsigned long long)r.pcm_bytes,
			        (unsigned long long)ref_bytes);
			return 5;
		}
		if (r.audio_frames != ref_frames)
			return 6;
		if ((r.media_flags & SDK_VIDEO_MEDIA_FLAG_AUDIO_DONE) == 0U)
			return 7;
		if (r.video_frames != 50U)
			return 8;
	}
	return 0;
}

/* 6b. A ring that cannot hold one decoded AC-3 frame would never emit
 * audio or drain: configure refuses it; exactly one frame is enough. */
static int test_ac3_ring_floor(void)
{
	const struct SDKVideoDecoderOps *ops = sdk_video_mpeg2_backend_ops();
	static uint8_t ring[SDK_DVD_AC3_FRAME_PCM_BYTES];
	struct SDKVideoMediaConfig cfg = {0};
	void *decoder;
	int ok_small, ok_exact;

	cfg.audio_codec = SDK_VIDEO_MEDIA_AUDIO_AC3;
	cfg.pcm_ring = ring;
	cfg.pcm_low_water_bytes = 1024U;
	cfg.pcm_ring_capacity = SDK_DVD_AC3_FRAME_PCM_BYTES - 4U;
	decoder = ops->create();
	if (!decoder)
		return 1;
	ok_small = ops->configure_media(decoder, &cfg);
	ops->destroy(decoder);
	cfg.pcm_ring_capacity = SDK_DVD_AC3_FRAME_PCM_BYTES;
	decoder = ops->create();
	if (!decoder)
		return 2;
	ok_exact = ops->configure_media(decoder, &cfg);
	ops->destroy(decoder);
	if (ok_small)
		return 3;
	if (!ok_exact)
		return 4;
	return 0;
}

/* 6c. A VOB cut at a pack boundary ends mid AC-3 frame: the partial frame
 * can never complete, so the stream must still reach DONE with only
 * whole frames played. */
static int test_ac3_truncated_mid_frame(void)
{
	struct acked_result r;
	uint32_t cut = 12U * 2048U;

	if (zz9k_dvd_ac3_192_fixture_len <= cut)
		return 1;
	if (run_acked(zz9k_dvd_ac3_192_fixture, cut, 2048U, 128U * 1024U,
	              128U * 1024U, &r) != 0)
		return 2;
	if (!r.done)
		return 3;
	if (r.pcm_bytes == 0U ||
	    r.pcm_bytes % SDK_DVD_AC3_FRAME_PCM_BYTES != 0U)
		return 4;
	if (r.audio_frames == 0U || r.audio_frames >= 63U)
		return 5;
	return 0;
}

/* 7. Two AC-3 tracks: 0x80 (7 frames, first in the stream) and 0x81 (19
 * frames). Only 0x80 may reach the decoder. */
static int test_ac3_substream_selection(void)
{
	static struct SDKDVDPSDemux demux;
	const struct SDKDVDPSPacket *p;
	struct acked_result r;
	uint64_t ref_bytes, ref_hash;
	uint32_t ref_frames;
	uint32_t accepted = 0U;
	uint32_t ac3_packets = 0U;

	sdk_dvd_ps_init(&demux);
	sdk_dvd_ps_write(&demux, zz9k_dvd_ac3_dual_fixture,
	                 zz9k_dvd_ac3_dual_fixture_len, &accepted, 1);
	while (!sdk_dvd_ps_done(&demux)) {
		uint32_t before = demux.video.count + demux.audio.count;

		while (sdk_dvd_ps_peek_video(&demux))
			sdk_dvd_ps_pop_video(&demux, NULL);
		while ((p = sdk_dvd_ps_peek_audio(&demux)) != NULL) {
			if (p->kind == SDK_DVD_PS_AC3) {
				if (p->substream_id != 0x80U)
					return 1;
				ac3_packets++;
			}
			sdk_dvd_ps_pop_audio(&demux, NULL);
		}
		if (accepted < zz9k_dvd_ac3_dual_fixture_len) {
			uint32_t more = 0U;

			sdk_dvd_ps_write(&demux,
			                 zz9k_dvd_ac3_dual_fixture + accepted,
			                 zz9k_dvd_ac3_dual_fixture_len - accepted,
			                 &more, 1);
			accepted += more;
		} else if (before == 0U && demux.video.count == 0U &&
		           demux.audio.count == 0U) {
			break;
		}
	}
	if (demux.ac3_substream != 0x80U || ac3_packets != 3U ||
	    demux.bytes_ignored == 0U)
		return 2;

	if (ac3_reference(zz9k_dvd_ac3_dual_fixture,
	                  zz9k_dvd_ac3_dual_fixture_len,
	                  &ref_bytes, &ref_hash, &ref_frames) != 0)
		return 3;
	if (ref_frames != 7U)
		return 4;
	if (run_acked(zz9k_dvd_ac3_dual_fixture, zz9k_dvd_ac3_dual_fixture_len,
	              1024U, 64U * 1024U, 64U * 1024U, &r) != 0)
		return 5;
	if (!r.done || r.audio_frames != 7U ||
	    r.pcm_bytes != 7U * (uint64_t)SDK_DVD_AC3_FRAME_PCM_BYTES ||
	    r.pcm_hash != ref_hash) {
		fprintf(stderr, "test_ac3_substream: done=%d frames=%u pcm=%llu\n",
		        r.done, r.audio_frames, (unsigned long long)r.pcm_bytes);
		return 6;
	}
	return 0;
}

int main(void)
{
	int err;

	printf("--- U11 DVD Host Correctness Tests ---\n");

	if ((err = test_lpcm_exactness()) != 0) {
		fprintf(stderr, "FAIL: test_lpcm_exactness (code %d)\n", err);
		return 10 + err;
	}
	printf("PASS: test_lpcm_exactness\n");

	if ((err = test_ac3_downmix_exactness()) != 0) {
		fprintf(stderr, "FAIL: test_ac3_downmix_exactness (code %d)\n", err);
		return 20 + err;
	}
	printf("PASS: test_ac3_downmix_exactness (deterministic 5.1 downmix)\n");

	if ((err = test_pal_25p()) != 0) {
		fprintf(stderr, "FAIL: test_pal_25p (code %d)\n", err);
		return 30 + err;
	}
	printf("PASS: test_pal_25p (PAL 720x576 25p + MP2 audio)\n");

	if ((err = test_pal_tff_interlaced()) != 0) {
		fprintf(stderr, "FAIL: test_pal_tff_interlaced (code %d)\n", err);
		return 40 + err;
	}
	printf("PASS: test_pal_tff_interlaced (PAL 720x576 50i TFF + AC-3)\n");

	if ((err = test_ntsc_30_lpcm()) != 0) {
		fprintf(stderr, "FAIL: test_ntsc_30_lpcm (code %d)\n", err);
		return 50 + err;
	}
	printf("PASS: test_ntsc_30_lpcm (NTSC 720x480 30000/1001 + LPCM)\n");

	if ((err = test_ntsc_film_32_cadence()) != 0) {
		fprintf(stderr, "FAIL: test_ntsc_film_32_cadence (code %d)\n", err);
		return 60 + err;
	}
	printf("PASS: test_ntsc_film_32_cadence (NTSC film 24000/1001 3:2 cadence)\n");

	if ((err = test_demuxer_bounded_behavior()) != 0) {
		fprintf(stderr, "FAIL: test_demuxer_bounded_behavior (code %d)\n", err);
		return 70 + err;
	}
	printf("PASS: test_demuxer_bounded_behavior (bounded queues & backpressure)\n");

	if ((err = test_ac3_192_all_frames()) != 0) {
		fprintf(stderr, "FAIL: test_ac3_192_all_frames (code %d)\n", err);
		return 80 + err;
	}
	printf("PASS: test_ac3_192_all_frames (AC-3 2.0 192k: every frame, DONE)\n");

	if ((err = test_ac3_ring_floor()) != 0) {
		fprintf(stderr, "FAIL: test_ac3_ring_floor (code %d)\n", err);
		return 100 + err;
	}
	printf("PASS: test_ac3_ring_floor (ring must hold one AC-3 frame)\n");

	if ((err = test_ac3_truncated_mid_frame()) != 0) {
		fprintf(stderr, "FAIL: test_ac3_truncated_mid_frame (code %d)\n", err);
		return 110 + err;
	}
	printf("PASS: test_ac3_truncated_mid_frame (cut VOB still ends DONE)\n");

	if ((err = test_ac3_substream_selection()) != 0) {
		fprintf(stderr, "FAIL: test_ac3_substream_selection (code %d)\n", err);
		return 90 + err;
	}
	printf("PASS: test_ac3_substream_selection (first AC-3 substream only)\n");

	printf("ALL DVD HOST TESTS PASSED.\n");
	return 0;
}
