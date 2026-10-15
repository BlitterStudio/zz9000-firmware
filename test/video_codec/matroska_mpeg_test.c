/*
 * Matroska input regression tests for the libmpeg2 backend.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "sdk_video_backend.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mpeg1_ps_fixture.inc"
#include "mpeg1_mp2_ps_fixture.inc"

static int failures;

static void check(int condition, const char *what)
{
	if (!condition) {
		fprintf(stderr, "FAIL: %s\n", what);
		failures++;
	}
}

struct buf {
	uint8_t *p;
	uint32_t n;
	uint32_t cap;
};

static void put(struct buf *b, const void *p, uint32_t n)
{
	uint8_t *next;
	uint32_t cap;

	if (b->n + n > b->cap) {
		cap = b->cap ? b->cap : 256U;
		while (cap < b->n + n)
			cap *= 2U;
		next = realloc(b->p, cap);
		if (!next) {
			fprintf(stderr, "out of memory\n");
			exit(2);
		}
		b->p = next;
		b->cap = cap;
	}
	if (n != 0U)
		memcpy(b->p + b->n, p, n);
	b->n += n;
}

static void byte(struct buf *b, uint8_t value)
{
	put(b, &value, 1U);
}

static void id(struct buf *b, uint32_t value)
{
	uint8_t bytes[4];
	uint32_t n = 1U;
	uint32_t i;

	while (n < 4U && (value >> (n * 8U)))
		n++;
	for (i = 0U; i < n; i++)
		bytes[i] = (uint8_t)(value >> (8U * (n - i - 1U)));
	put(b, bytes, n);
}

static void size(struct buf *b, uint32_t value)
{
	uint32_t n = 1U;
	uint32_t i;
	uint8_t bytes[4];

	while (n < 4U && value >= ((UINT32_C(1) << (7U * n)) - 1U))
		n++;
	for (i = 0U; i < n; i++)
		bytes[i] = (uint8_t)(value >> (8U * (n - i - 1U)));
	bytes[0] |= (uint8_t)(UINT32_C(1) << (8U - n));
	put(b, bytes, n);
}

static void elem(struct buf *b, uint32_t element_id, const void *p, uint32_t n)
{
	id(b, element_id);
	size(b, n);
	put(b, p, n);
}

static void u8(struct buf *b, uint32_t element_id, uint8_t value)
{
	elem(b, element_id, &value, 1U);
}

static void u16(struct buf *b, uint32_t element_id, uint16_t value)
{
	uint8_t bytes[2] = {(uint8_t)(value >> 8), (uint8_t)value};

	elem(b, element_id, bytes, sizeof(bytes));
}
static void u32(struct buf *b, uint32_t element_id, uint32_t value)
{
	uint8_t bytes[4] = {(uint8_t)(value >> 24), (uint8_t)(value >> 16),
		(uint8_t)(value >> 8), (uint8_t)value};

	elem(b, element_id, bytes, sizeof(bytes));
}

static void nest(struct buf *to, uint32_t element_id, struct buf *body)
{
	id(to, element_id);
	size(to, body->n);
	put(to, body->p, body->n);
	free(body->p);
	memset(body, 0, sizeof(*body));
}

static void add_video_track(struct buf *tracks, int enabled, uint8_t number)
{
	struct buf track = {0};
	struct buf video = {0};

	u8(&track, 0xD7, number);
	u8(&track, 0x83, 1U);
	if (!enabled)
		u8(&track, 0xB9, 0U);
	elem(&track, 0x86, "V_MPEG1", 7U);
	u16(&video, 0xB0, 32U);
	u16(&video, 0xBA, 16U);
	nest(&track, 0xE0, &video);
	nest(tracks, 0xAE, &track);
}

static void add_audio_track(struct buf *tracks, const char *codec,
	uint32_t channels, uint32_t bit_depth, int channels_present, int enabled,
	uint32_t default_duration_ns)
{
	struct buf track = {0};
	struct buf audio = {0};
	static const uint8_t rate_48000[] = {0x47, 0x3b, 0x80, 0x00};

	u8(&track, 0xD7, 2U);
	u8(&track, 0x83, 2U);
	if (!enabled)
		u8(&track, 0xB9, 0U);
	elem(&track, 0x86, codec, (uint32_t)strlen(codec));
	if (default_duration_ns != 0U)
		u32(&track, 0x23E383, default_duration_ns);
	elem(&audio, 0xB5, rate_48000, sizeof(rate_48000));
	if (channels_present)
		u8(&audio, 0x9F, (uint8_t)channels);
	if (bit_depth <= UINT8_MAX)
		u8(&audio, 0x6264, (uint8_t)bit_depth);
	else
		u32(&audio, 0x6264, bit_depth);
	nest(&track, 0xE1, &audio);
	nest(tracks, 0xAE, &track);
}

static void add_block(struct buf *cluster, uint8_t track, uint16_t timecode,
	const uint8_t *data, uint32_t length)
{
	struct buf block = {0};

	byte(&block, (uint8_t)(0x80U | track));
	byte(&block, (uint8_t)(timecode >> 8));
	byte(&block, (uint8_t)timecode);
	byte(&block, track == 1U ? 0x80U : 0x00U);
	put(&block, data, length);
	nest(cluster, 0xA3, &block);
}

static void add_xiph_laced_audio_block(struct buf *cluster, uint16_t timecode,
	const uint8_t *data, uint32_t length, uint8_t frames,
	uint8_t frame_copies)
{
	struct buf block = {0};
	uint32_t frame_length = length * frame_copies;
	uint32_t i, j;

	byte(&block, 0x82U);
	byte(&block, (uint8_t)(timecode >> 8));
	byte(&block, (uint8_t)timecode);
	byte(&block, 0x02U);
	byte(&block, (uint8_t)(frames - 1U));
	for (i = 0U; i + 1U < frames; i++) {
		uint32_t remaining = frame_length;

		while (remaining >= UINT8_MAX) {
			byte(&block, UINT8_MAX);
			remaining -= UINT8_MAX;
		}
		byte(&block, (uint8_t)remaining);
	}
	for (i = 0U; i < frames; i++)
		for (j = 0U; j < frame_copies; j++)
			put(&block, data, length);
	nest(cluster, 0xA3, &block);
}

static struct buf make_file_ex(const uint8_t *video, uint32_t video_length,
	const char *audio_codec, uint32_t channels, uint32_t bit_depth,
	const uint8_t *audio, uint32_t audio_length, int video_enabled,
	int audio_enabled, int channels_present, uint32_t default_duration_ns,
	int audio_laced, uint32_t timecode_scale, uint16_t audio_timecode)
{
	struct buf file = {0};
	struct buf ebml = {0};
	struct buf segment = {0};
	struct buf info = {0};
	struct buf tracks = {0};
	struct buf cluster = {0};
	u32(&info, 0x2AD7B1, timecode_scale);

	elem(&ebml, 0x4282, "matroska", 8U);
	nest(&file, 0x1A45DFA3, &ebml);
	nest(&segment, 0x1549A966, &info);
	add_video_track(&tracks, video_enabled, 1U);
	if (audio_codec)
		add_audio_track(&tracks, audio_codec, channels, bit_depth,
			channels_present, audio_enabled, default_duration_ns);
	nest(&segment, 0x1654AE6B, &tracks);
	u8(&cluster, 0xE7, 0U);
	if (video_length)
		add_block(&cluster, 1U, 0U, video, video_length);
	if (audio_laced)
		add_xiph_laced_audio_block(&cluster, audio_timecode, audio, audio_length,
			(uint8_t)audio_laced, 2U);
	else if (audio_length)
		add_block(&cluster, 2U, 0U, audio, audio_length);
	nest(&segment, 0x1F43B675, &cluster);
	id(&file, 0x18538067);
	byte(&file, 0xffU);
	put(&file, segment.p, segment.n);
	free(segment.p);
	return file;
}

static struct buf make_file(const uint8_t *video, uint32_t video_length,
	const char *audio_codec, uint32_t channels, uint32_t bit_depth,
	const uint8_t *audio, uint32_t audio_length)
{
	return make_file_ex(video, video_length, audio_codec, channels, bit_depth,
		audio, audio_length, 1, 1, 1, 0U, 0, 1000000U, 0U);
}

static struct buf make_replacement_video_file(void)
{
	struct buf file = {0};
	struct buf ebml = {0};
	struct buf segment = {0};
	struct buf info = {0};
	struct buf tracks = {0};
	struct buf cluster = {0};

	elem(&ebml, 0x4282, "matroska", 8U);
	nest(&file, 0x1A45DFA3, &ebml);
	u32(&info, 0x2AD7B1, 1000000U);
	nest(&segment, 0x1549A966, &info);
	add_video_track(&tracks, 0, 1U);
	add_video_track(&tracks, 1, 3U);
	nest(&segment, 0x1654AE6B, &tracks);
	u8(&cluster, 0xE7, 0U);
	nest(&segment, 0x1F43B675, &cluster);
	id(&file, 0x18538067);
	byte(&file, 0xffU);
	put(&file, segment.p, segment.n);
	free(segment.p);
	return file;
}
/* Return concatenated payloads for PES stream_id. The fixture is a
 * program stream, but Matroska blocks carry elementary streams. */
static int pes_payloads(const uint8_t *in, uint32_t length, uint8_t stream_id,
	struct buf *out)
{
	uint32_t i;
	unsigned found = 0U;

	for (i = 0U; i + 6U <= length; i++) {
		uint32_t cursor;
		uint32_t end;
		uint32_t packet_length;
		if (in[i] != 0U || in[i + 1U] != 0U || in[i + 2U] != 1U ||
		    in[i + 3U] != stream_id)
			continue;
		packet_length = ((uint32_t)in[i + 4U] << 8) | in[i + 5U];
		end = i + 6U + packet_length;
		if (packet_length == 0U || end > length)
			continue;
		cursor = i + 6U;
		while (cursor < end && in[cursor] == 0xffU)
			cursor++;
		if (cursor < end && (in[cursor] & 0xc0U) == 0x40U)
			cursor += 2U;
		if (cursor >= end)
			continue;
		if ((in[cursor] & 0xf0U) == 0x20U)
			cursor += 5U;
		else if ((in[cursor] & 0xf0U) == 0x30U)
			cursor += 10U;
		else if (in[cursor] == 0x0fU)
			cursor++;
		else
			continue;
		if (cursor > end)
			continue;
		put(out, in + cursor, end - cursor);
		found++;
		i = end - 1U;
	}
	return found != 0U;
}

static const struct SDKVideoDecoderOps *mkv_ops(void)
{
	return sdk_video_backend_find(SDK_VIDEO_CODEC_MPEG1,
		SDK_VIDEO_CONTAINER_WEBM);
}

static void *open_stream(const struct buf *file, uint32_t audio_codec,
	uint8_t *ring, uint32_t ring_capacity, uint32_t high_water, int eof)
{
	const struct SDKVideoDecoderOps *ops = mkv_ops();
	struct SDKVideoMediaConfig config;
	void *decoder;
	uint32_t accepted = 0U;

	check(ops != 0, "Matroska MPEG-1 backend is registered");
	if (!ops)
		return 0;
	decoder = ops->create(0U);
	check(decoder != 0, "Matroska MPEG-1 decoder creates");
	if (!decoder)
		return 0;
	memset(&config, 0, sizeof(config));
	config.audio_codec = audio_codec;
	config.pcm_ring = ring;
	config.pcm_ring_capacity = ring_capacity;
	config.pcm_low_water_bytes = 0U;
	config.pcm_high_water_bytes = high_water;
	check(ops->configure_media(decoder, &config), "media config is accepted");
	check(ops->write(decoder, file->p, file->n, eof, &accepted) ==
		SDK_VIDEO_BACKEND_WRITE_OK && accepted == file->n,
		"complete Matroska stream is accepted");
	return decoder;
}

static void test_clean_eof_drains_delayed_picture(void)
{
	const struct SDKVideoDecoderOps *ops = mkv_ops();
	struct buf video = {0};
	struct buf file;
	struct SDKVideoDecodedFrame frame;
	void *decoder;
	unsigned frames = 0U;
	unsigned guard;
	int rc = SDK_VIDEO_BACKEND_NEED_INPUT;

	check(pes_payloads(zz9k_mpeg1_ps_fixture,
		(uint32_t)sizeof(zz9k_mpeg1_ps_fixture), 0xe0U, &video),
		"video fixture supplies elementary stream");
	file = make_file(video.p, video.n, 0, 0U, 0U, 0, 0U);
	decoder = open_stream(&file, SDK_VIDEO_MEDIA_AUDIO_NONE, 0, 0U, 0U, 1);
	if (decoder) {
		for (guard = 0U; guard < 128U; guard++) {
			rc = ops->decode(decoder, &frame);
			if (rc == SDK_VIDEO_BACKEND_FRAME)
				frames++;
			else if (rc == SDK_VIDEO_BACKEND_DONE)
				break;
			else if (rc != SDK_VIDEO_BACKEND_PROGRESS)
				break;
		}
		check(rc == SDK_VIDEO_BACKEND_DONE,
			"clean Matroska EOF ends DONE, not NEED_INPUT");
		check(frames == 6U, "clean EOF delivers every delayed picture");
		ops->destroy(decoder);
	}
	free(file.p);
	free(video.p);
}

static void test_pcm_mono_is_stereo_and_keeps_duration(void)
{
	const struct SDKVideoDecoderOps *ops = mkv_ops();
	uint8_t ring[4096];
	uint8_t pcm[480U * 2U];
	struct buf file;
	struct SDKVideoMediaInfo info;
	struct SDKVideoDecodedFrame frame;
	void *decoder;
	unsigned guard;
	int rc = SDK_VIDEO_BACKEND_NEED_INPUT;

	memset(pcm, 0x5a, sizeof(pcm));
	file = make_file(0, 0U, "A_PCM/INT/LIT", 1U, 16U, pcm, sizeof(pcm));
	decoder = open_stream(&file, SDK_VIDEO_MEDIA_AUDIO_LPCM, ring,
		sizeof(ring), sizeof(ring), 1);
	if (decoder) {
		for (guard = 0U; guard < 32U; guard++) {
			rc = ops->decode(decoder, &frame);
			if (rc == SDK_VIDEO_BACKEND_DONE)
				break;
			if (rc != SDK_VIDEO_BACKEND_PROGRESS &&
			    rc != SDK_VIDEO_BACKEND_FRAME)
				break;
		}
		check(rc == SDK_VIDEO_BACKEND_DONE, "mono PCM stream completes");
		check(ops->get_media_info(decoder, &info) &&
			info.sample_rate == 48000U && info.channels == 2U &&
			info.sample_format == SDK_VIDEO_MEDIA_SAMPLE_S16BE,
			"mono PCM reports emitted stereo format");
		check(ops->get_media_info(decoder, &info) &&
			info.pcm_produced == 480U * SDK_VIDEO_MEDIA_PCM_FRAME_BYTES,
			"mono PCM duration matches 480 input samples");
		ops->destroy(decoder);
	}
	free(file.p);
}

static void test_mp2_resumes_after_acknowledgement(void)
{
	const struct SDKVideoDecoderOps *ops = mkv_ops();
	uint8_t ring[9216];
	struct buf audio = {0};
	struct buf staged = {0};
	struct buf file;
	struct SDKVideoMediaInfo before;
	struct SDKVideoMediaInfo after;
	struct SDKVideoDecodedFrame frame;
	void *decoder;
	unsigned guard;
	unsigned repeat;
	uint32_t accepted = 1U;
	int rc = SDK_VIDEO_BACKEND_NEED_INPUT;

	check(pes_payloads(zz9k_mpeg1_mp2_ps_fixture,
		(uint32_t)sizeof(zz9k_mpeg1_mp2_ps_fixture), 0xc0U, &audio),
		"MP2 fixture supplies elementary stream");
	for (repeat = 0U; repeat < 8U; repeat++)
		put(&staged, audio.p, audio.n);
	file = make_file(0, 0U, "A_MPEG/L2", 2U, 16U, staged.p, staged.n);
	decoder = open_stream(&file, SDK_VIDEO_MEDIA_AUDIO_MP2, ring,
		sizeof(ring), sizeof(ring), 0);
	if (decoder) {
		for (guard = 0U; guard < 32U; guard++) {
			rc = ops->decode(decoder, &frame);
			if (rc == SDK_VIDEO_BACKEND_BACKPRESSURE)
				break;
			if (rc == SDK_VIDEO_BACKEND_DONE || rc == SDK_VIDEO_BACKEND_ERROR ||
			    rc == SDK_VIDEO_BACKEND_NEED_INPUT)
				break;
		}
		check(rc == SDK_VIDEO_BACKEND_BACKPRESSURE &&
			ops->get_media_info(decoder, &before) && before.pcm_produced != 0U,
			"staged MP2 reaches PCM backpressure");
	check(ops->ack_media(decoder, before.pcm_produced),
		"PCM acknowledgement is accepted");
	rc = ops->decode(decoder, &frame);
	check(rc == SDK_VIDEO_BACKEND_BACKPRESSURE &&
		ops->get_media_info(decoder, &after) &&
		after.pcm_produced > before.pcm_produced,
		"staged MP2 resumes after PCM acknowledgement");
	if (rc == SDK_VIDEO_BACKEND_BACKPRESSURE)
		(void)ops->ack_media(decoder, after.pcm_produced);
	check(ops->write(decoder, 0, 0U, 1, &accepted) ==
		SDK_VIDEO_BACKEND_WRITE_OK && accepted == 0U,
		"MP2 EOF is accepted after resuming");
	for (guard = 0U; guard < 128U; guard++) {
			rc = ops->decode(decoder, &frame);
			if (rc == SDK_VIDEO_BACKEND_BACKPRESSURE &&
			    ops->get_media_info(decoder, &after)) {
				if (!ops->ack_media(decoder, after.pcm_produced))
					break;
				continue;
			}
			if (rc == SDK_VIDEO_BACKEND_DONE)
				break;
			if (rc != SDK_VIDEO_BACKEND_PROGRESS && rc != SDK_VIDEO_BACKEND_FRAME)
				break;
		}
		check(rc == SDK_VIDEO_BACKEND_DONE,
			"staged MP2 resumes after PCM acknowledgement and completes");
		ops->destroy(decoder);
	}
	free(file.p);
	free(audio.p);
	free(staged.p);
}

static void test_pcm_24bit_is_unsupported(void)
{
	const struct SDKVideoDecoderOps *ops = mkv_ops();
	uint8_t ring[4096];
	uint8_t pcm[6] = {0};
	struct buf file;
	struct SDKVideoMediaConfig config;
	uint32_t accepted = 0U;
	void *decoder;

	file = make_file(0, 0U, "A_PCM/INT/LIT", 1U, 24U, pcm, sizeof(pcm));
	decoder = ops ? ops->create(0U) : 0;
	check(decoder != 0, "24-bit test decoder creates");
	if (decoder) {
		memset(&config, 0, sizeof(config));
		config.audio_codec = SDK_VIDEO_MEDIA_AUDIO_LPCM;
		config.pcm_ring = ring;
		config.pcm_ring_capacity = sizeof(ring);
		config.pcm_high_water_bytes = sizeof(ring);
		check(ops->configure_media(decoder, &config), "24-bit media config is accepted");
		check(ops->write(decoder, file.p, file.n, 1, &accepted) ==
			SDK_VIDEO_BACKEND_WRITE_UNSUPPORTED,
			"24-bit PCM track is refused instead of treated as 16-bit");
		ops->destroy(decoder);
	}
	free(file.p);
}
static void expect_write_unsupported(const struct buf *file, uint32_t audio_codec,
	const char *what)
{
	const struct SDKVideoDecoderOps *ops = mkv_ops();
	struct SDKVideoMediaConfig config;
	uint8_t ring[4096];
	uint32_t accepted = 0U;
	void *decoder = ops ? ops->create(0U) : 0;

	check(decoder != 0, what);
	if (!decoder)
		return;
	memset(&config, 0, sizeof(config));
	config.audio_codec = audio_codec;
	if (audio_codec != SDK_VIDEO_MEDIA_AUDIO_NONE) {
		config.pcm_ring = ring;
		config.pcm_ring_capacity = sizeof(ring);
		config.pcm_high_water_bytes = sizeof(ring);
	}
	check(ops->configure_media(decoder, &config), what);
	check(ops->write(decoder, file->p, file->n, 1, &accepted) ==
		SDK_VIDEO_BACKEND_WRITE_UNSUPPORTED, what);
	ops->destroy(decoder);
}

static void test_disabled_tracks_are_ineligible(void)
{
	uint8_t pcm[4] = {0};
	struct buf video_disabled = make_file_ex(0, 0U, 0, 0U, 0U, 0, 0U,
		0, 1, 1, 0U, 0, 1000000U, 0U);
	struct buf audio_disabled = make_file_ex(0, 0U, "A_PCM/INT/LIT", 1U,
		16U, pcm, sizeof(pcm), 1, 0, 1, 0U, 0, 1000000U, 0U);

	expect_write_unsupported(&video_disabled, SDK_VIDEO_MEDIA_AUDIO_NONE,
		"disabled video track is ineligible");
	expect_write_unsupported(&audio_disabled, SDK_VIDEO_MEDIA_AUDIO_LPCM,
		"disabled audio track is ineligible");
	free(video_disabled.p);
	free(audio_disabled.p);
}

static void test_enabled_replacement_video_is_selected(void)
{
	struct buf file = make_replacement_video_file();
	void *decoder = open_stream(&file, SDK_VIDEO_MEDIA_AUDIO_NONE,
		0, 0U, 0U, 1);

	check(decoder != 0,
		"enabled video replaces a disabled track with a distinct number");
	if (decoder)
		mkv_ops()->destroy(decoder);
	free(file.p);
}

static void test_pcm_track_defaults_and_rejects_oversized_depth(void)
{
	uint8_t ring[4096];
	uint8_t pcm[4] = {0};
	struct buf mono = make_file_ex(0, 0U, "A_PCM/INT/LIT", 1U, 16U,
		pcm, sizeof(pcm), 1, 1, 0, 0U, 0, 1000000U, 0U);
	struct buf oversized = make_file_ex(0, 0U, "A_PCM/INT/LIT", 1U,
		65552U, pcm, sizeof(pcm), 1, 1, 1, 0U, 0, 1000000U, 0U);
	struct SDKVideoMediaInfo info;
	const struct SDKVideoDecoderOps *ops = mkv_ops();
	void *decoder = open_stream(&mono, SDK_VIDEO_MEDIA_AUDIO_LPCM, ring,
		sizeof(ring), sizeof(ring), 1);

	if (decoder) {
		struct SDKVideoDecodedFrame frame;
		unsigned guard;
		for (guard = 0U; guard < 16U; guard++)
			if (ops->decode(decoder, &frame) == SDK_VIDEO_BACKEND_DONE)
				break;
		check(ops->get_media_info(decoder, &info) && info.channels == 2U,
			"omitted Audio Channels defaults PCM to mono");
		ops->destroy(decoder);
	}
	expect_write_unsupported(&oversized, SDK_VIDEO_MEDIA_AUDIO_LPCM,
		"BitDepth 65552 is retained and rejects PCM");
	free(mono.p);
	free(oversized.p);
}

static void test_laced_mp2_advances_audio_pts(void)
{
	const struct SDKVideoDecoderOps *ops = mkv_ops();
	struct buf video = {0};
	struct buf audio = {0};
	struct buf file;
	struct SDKVideoMediaInfo info;
	struct SDKVideoDecodedFrame frame;
	uint8_t ring[32768];
	void *decoder;
	unsigned guard;
	unsigned acknowledgements = 0U;
	uint64_t first_backpressure_pts = 0U;
	uint64_t final_audio_pts = 0U;
	uint64_t final_pcm = 0U;
	int saw_block_pts = 0;
	int saw_backpressure = 0;
	int saw_done = 0;
	int bad_flags = 0;

	check(pes_payloads(zz9k_mpeg1_ps_fixture,
		(uint32_t)sizeof(zz9k_mpeg1_ps_fixture), 0xe0U, &video),
		"laced MP2 timeline video fixture supplies elementary stream");
	check(pes_payloads(zz9k_mpeg1_mp2_ps_fixture,
		(uint32_t)sizeof(zz9k_mpeg1_mp2_ps_fixture), 0xc0U, &audio),
		"laced MP2 timeline audio fixture supplies elementary stream");
	/* 24 laces each carry two MP2 frames, spanning more than a second. The
	 * duration matches each decoded lace, while the small PCM ring makes it
	 * resume over multiple calls. */
	file = make_file_ex(video.p, video.n, "A_MPEG/L2", 2U, 16U,
		audio.p, audio.n, 1, 1, 1, 835918367U, 24, 2000000U, 17U);
	decoder = open_stream(&file, SDK_VIDEO_MEDIA_AUDIO_MP2, ring,
		sizeof(ring), sizeof(ring), 1);
	if (decoder) {
		for (guard = 0U; guard < 512U; guard++) {
			int rc = ops->decode(decoder, &frame);

			if (ops->get_media_info(decoder, &info)) {
				if (info.raw_pts == 3060U)
					saw_block_pts = 1;
				if ((info.flags & (SDK_VIDEO_MEDIA_FLAG_DISCONTINUITY |
						   SDK_VIDEO_MEDIA_FLAG_REBASED)) != 0U)
					bad_flags = 1;
				final_audio_pts = info.audio_pts;
				final_pcm = info.pcm_produced;
				if (rc == SDK_VIDEO_BACKEND_BACKPRESSURE &&
				    info.pcm_produced != 0U) {
					if (!saw_backpressure)
						first_backpressure_pts = info.audio_pts;
					saw_backpressure = 1;
					check(ops->ack_media(decoder, info.pcm_produced),
						"laced MP2 acknowledges blocked PCM");
					acknowledgements++;
				}
			}
			if (rc == SDK_VIDEO_BACKEND_DONE) {
				saw_done = 1;
				break;
			}
		}
		check(saw_block_pts,
			"laced MP2 anchors the first frame at the scaled block PTS");
		check(saw_backpressure && acknowledgements != 0U,
			"laced MP2 resumes after PCM-ring acknowledgement");
		check(first_backpressure_pts < 90000U,
			"blocked laces do not rebase the audio clock one second ahead");
		check(!bad_flags,
			"laced MP2 timestamps do not rebase the audio timeline");
		check(saw_done && final_pcm / 4U > 48000U && info.sample_rate != 0U &&
		      final_audio_pts == 3060U +
		      (final_pcm / 4U) * 90000U / info.sample_rate,
			"later laces advance the audio clock by their decoded samples");
		ops->destroy(decoder);
	}
	free(file.p);
	free(audio.p);
	free(video.p);
}

int main(void)
{
	test_clean_eof_drains_delayed_picture();
	test_pcm_mono_is_stereo_and_keeps_duration();
	test_mp2_resumes_after_acknowledgement();
	test_pcm_24bit_is_unsupported();
	test_disabled_tracks_are_ineligible();
	test_enabled_replacement_video_is_selected();
	test_pcm_track_defaults_and_rejects_oversized_depth();
	test_laced_mp2_advances_audio_pts();
	if (failures != 0)
		return 1;
	puts("Matroska MPEG regressions passed");
	return 0;
}
