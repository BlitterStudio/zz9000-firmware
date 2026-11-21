/*
 * Behavioural test for the MP3 stream decode pass bound: one decode call
 * must be bounded by the ~2 ms service cap (sdk_audio_codec_io.h) even
 * when the client's high-water budget is the whole PCM ring, and the
 * stream must finish across passes with byte-identical PCM. Drives the
 * production loop in sdk_audio_stream.c -- the exact function
 * sdk_mailbox.c dispatches MP3 streams to -- over a real 128 kbps stereo
 * MP3 (ffmpeg sine, committed under fixtures/).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_stream_drain.h"
#include "sdk_audio_codec_io.h"
#include "sdk_audio_stream.h"

#define IN_CAP   (128U * 1024U)
#define PCM_CAP  (256U * 1024U)
#define MAX_OUT  (1024U * 1024U)

/* 1152-sample stereo S16 frame, the production frame_pcm_bytes quantum. */
#define FRAME_PCM (1152U * 2U * 2U)
#define EXPECTED_PASS_PCM_CAP (24U * 1024U)


static int failures;

#define CHECK(cond, ...) do { \
	if (!(cond)) { \
		failures++; \
		printf("FAIL %s:%d: ", __FILE__, __LINE__); \
		printf(__VA_ARGS__); \
		printf("\n"); \
	} \
} while (0)

static struct SDKAudioStream g_stream;
static uint8_t g_in[IN_CAP];
static uint8_t g_pcm[PCM_CAP];
static uint8_t g_out[MAX_OUT];
static uint8_t g_ref[MAX_OUT];
static uint32_t g_out_len;
static uint8_t *g_mp3;
static uint32_t g_mp3_len;

static void load_mp3(const char *path)
{
	FILE *f = fopen(path, "rb");
	long n;

	if (!f) {
		printf("FATAL: cannot open %s\n", path);
		exit(2);
	}
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	g_mp3 = (uint8_t *)malloc(n > 0 ? (size_t)n : 1U);
	if (!g_mp3 || fread(g_mp3, 1U, (size_t)n, f) != (size_t)n) {
		printf("FATAL: cannot read %s\n", path);
		exit(2);
	}
	fclose(f);
	g_mp3_len = (uint32_t)n;
}

/* Configure the stream like the production BEGIN for an MHI-style
 * client: 16-KiB compressed ring, 256-KiB PCM ring, low water half the
 * ring, high water unset (the loop falls back to the full ring). */
static void stream_init(void)
{
	struct SDKAudioStream *s = &g_stream;

	memset(s, 0, sizeof(*s));
	memset(g_pcm, 0, sizeof(g_pcm));
	s->codec = SDK_AUDIO_CODEC_MP3;
	s->mp3_capacity = IN_CAP;
	s->pcm_capacity = PCM_CAP;
	s->mp3_ring_addr = (uintptr_t)g_in;
	s->pcm_ring_addr = (uintptr_t)g_pcm;
	s->low_water_bytes = PCM_CAP / 2U;
	s->sample_format = SDK_AUDIO_SAMPLE_FORMAT_S16LE;
	memcpy(g_in, g_mp3, g_mp3_len);
	s->input_length = g_mp3_len;
}

/* One production decode pass, then the consumer drains everything the
 * pass published (what AUDIO_STREAM_READ does for an unbound stream).
 * Returns the pass's produced bytes; appends them to g_out. */
static uint32_t pass_and_drain(void)
{
	struct SDKAudioStream *s = &g_stream;
	uint32_t before = s->pcm_ready_total;
	uint32_t delta;
	uint32_t i;
	uint32_t pos;

	(void)audio_stream_decode_mp3(s);
	delta = s->pcm_ready_total - before;
	pos = (s->pcm_ready_total - delta) % s->pcm_capacity;
	for (i = 0U; i < delta && g_out_len < MAX_OUT; i++) {
		g_out[g_out_len++] = g_pcm[pos];
		if (++pos == s->pcm_capacity)
			pos = 0U;
	}
	s->pcm_consumed_total = s->pcm_ready_total;
	return delta;
}

/* Find the four-frame audio prefix in tone.mp3. Its first complete MPEG
 * frame carries the Xing metadata and is consumed without becoming PCM in
 * the production path, so include it before the four PCM frames. */
static uint32_t input_for_output_frames(uint32_t wanted_frames)
{
	mp3dec_t decoder;
	mp3d_sample_t scratch[MINIMP3_MAX_SAMPLES_PER_FRAME];
	uint32_t offset = 0U;
	uint32_t frames = 0U;

	memset(&decoder, 0, sizeof(decoder));
	while (offset < g_mp3_len && frames < wanted_frames + 1U) {
		mp3dec_frame_info_t info;
		int samples;

		memset(&info, 0, sizeof(info));
		samples = mp3dec_decode_frame(
			&decoder, g_mp3 + offset, (int)(g_mp3_len - offset),
			scratch, &info);
		if (info.frame_bytes <= 0)
			return 0U;
		offset += (uint32_t)info.frame_bytes;
		if (samples > 0)
			frames++;
	}
	return frames == wanted_frames + 1U ? offset : 0U;
}

/* A DRAIN at an exact MP3 frame boundary has no native decoder tail. The
 * production MP3 pass must classify it complete, or the pump reschedules a
 * useless empty-input refill forever. */
static void test_drain_at_exact_frame_boundary(void)
{
	struct SDKAudioStream *s = &g_stream;
	uint32_t input_bytes = input_for_output_frames(4U);
	uint32_t produced;
	uint32_t pass;

	CHECK(input_bytes != 0U, "could not find four MP3 frames");
	if (input_bytes == 0U)
		return;
	stream_init();
	s->input_length = input_bytes;
	s->drain_requested = 1;
	g_out_len = 0U;
	produced = pass_and_drain();
	CHECK(produced == 4U * FRAME_PCM,
	      "four-frame drain produced %u bytes", produced);
	CHECK(s->frames_decoded == 4U && s->input_length == 0U,
	      "drain state frames=%u input=%u", s->frames_decoded,
	      s->input_length);
	CHECK(s->drain_input_complete,
	      "exact MP3 frame-boundary drain was not complete");
	for (pass = 0U; pass < 20U; pass++) {
		CHECK(!audio_stream_refill_may_run(
			      s->input_length, s->decode_complete, s->faulted, 0,
			      s->drain_input_complete, audio_stream_pcm_used(s),
			      s->low_water_bytes, s->pcm_written_total,
			      s->pcm_consumed_total, 0U, &s->refill_checkpoint),
		      "pump would schedule empty MP3 drain refill %u", pass);
	}
}

static void test_pass_cap(void)
{
	struct SDKAudioStream *s = &g_stream;
	uint32_t pass_bound = EXPECTED_PASS_PCM_CAP + FRAME_PCM;
	uint32_t ref_len;
	uint32_t total;
	uint32_t passes = 0U;
	uint32_t d;
	uint32_t guard;

	/* Reference: the same production loop, drained every pass, until it
	 * stops making progress (the compressed tail drops below the
	 * lookahead gate without EOF). */
	g_out_len = 0U;
	stream_init();
	ref_len = 0U;
	for (guard = 0U; guard < 1000U; guard++) {
		uint32_t made = pass_and_drain();

		ref_len += made;
		if (made == 0U)
			break;
	}
	CHECK(s->sample_rate == 44100U && s->channels == 2U,
	      "geometry %u/%u", s->sample_rate, s->channels);
	CHECK(ref_len >= 6U * FRAME_PCM && ref_len % FRAME_PCM == 0U,
	      "reference decoded %u bytes", ref_len);
	memcpy(g_ref, g_out, ref_len);

	/* Bounded pass: whole file buffered, empty PCM ring, full-ring
	 * high-water fallback. */
	stream_init();
	g_out_len = 0U;
	d = pass_and_drain();
	CHECK(d > 0U && d <= pass_bound,
	      "first pass decoded %u bytes (bound %u)", d, pass_bound);
	CHECK(!s->decode_complete, "first pass must not finish the stream");
	total = d;
	while (!s->decode_complete && passes++ < 1000U) {
		d = pass_and_drain();
		CHECK(d <= pass_bound,
		      "pass %u decoded %u bytes (bound %u)", passes, d,
		      pass_bound);
		total += d;
		if (d == 0U)
			break;
	}
	CHECK(g_out_len == ref_len && memcmp(g_out, g_ref, ref_len) == 0,
	      "capped decode produced %u of %u bytes, %s",
	      g_out_len, ref_len,
	      g_out_len == ref_len ? "differs" : "short");
	CHECK(passes >= ref_len / EXPECTED_PASS_PCM_CAP,
	      "stream finished in %u extra passes", passes);
}

/* ---- output_hz / output_channels reduction ---- */

static void swap16(uint8_t *data, uint32_t bytes)
{
	uint32_t i;

	for (i = 0U; i + 1U < bytes; i += 2U) {
		uint8_t t = data[i];

		data[i] = data[i + 1U];
		data[i + 1U] = t;
	}
}

static void test_begin_status(void)
{
	CHECK(audio_stream_reduce_begin_status(SDK_AUDIO_SAMPLE_FORMAT_S16LE,
	                                       0U, 0U) == SDK_STATUS_OK,
	      "zero fields");
	CHECK(audio_stream_reduce_begin_status(SDK_AUDIO_SAMPLE_FORMAT_S32BE,
	                                       0U, 0U) == SDK_STATUS_OK,
	      "zero fields with S32");
	CHECK(audio_stream_reduce_begin_status(SDK_AUDIO_SAMPLE_FORMAT_S16LE,
	                                       22050U, 1U) == SDK_STATUS_OK,
	      "S16LE 22050/1");
	CHECK(audio_stream_reduce_begin_status(SDK_AUDIO_SAMPLE_FORMAT_S16BE,
	                                       48000U, 2U) == SDK_STATUS_OK,
	      "S16BE 48000/2");
	CHECK(audio_stream_reduce_begin_status(SDK_AUDIO_SAMPLE_FORMAT_S16LE,
	                                       0U, 3U) == SDK_STATUS_UNSUPPORTED,
	      "three channels");
	CHECK(audio_stream_reduce_begin_status(SDK_AUDIO_SAMPLE_FORMAT_S32BE,
	                                       22050U, 0U) ==
	          SDK_STATUS_UNSUPPORTED,
	      "S32 with a rate limit");
	CHECK(audio_stream_reduce_begin_status(SDK_AUDIO_SAMPLE_FORMAT_S32BE,
	                                       0U, 1U) == SDK_STATUS_UNSUPPORTED,
	      "S32 with a channel request");
}

struct reduce_case {
	uint32_t hz;
	uint32_t channels;
	uint32_t want_hz;
	uint32_t want_channels;
};

static const struct reduce_case reduce_cases[] = {
	{ 22050U, 0U, 22050U, 2U },
	{ 0U, 1U, 44100U, 1U },
	{ 22050U, 1U, 22050U, 1U },
	{ 11025U, 0U, 11025U, 2U },
	{ 8000U, 1U, 11025U, 1U },
};

/* The MP3 pass with a reduction request writes exactly what the reducer
 * makes of the full-rate stream, in either byte order, and its bounded
 * pass still holds. */
static void test_mp3_reduced(void)
{
	uint32_t full_len;
	uint32_t guard;
	uint32_t k;
	uint32_t be;

	g_out_len = 0U;
	stream_init();
	full_len = 0U;
	for (guard = 0U; guard < 1000U; guard++) {
		uint32_t made = pass_and_drain();

		full_len += made;
		if (made == 0U)
			break;
	}
	memcpy(g_ref, g_out, full_len);

	for (k = 0U; k < sizeof(reduce_cases) / sizeof(reduce_cases[0]); k++) {
		const struct reduce_case *tc = &reduce_cases[k];

		for (be = 0U; be < 2U; be++) {
			struct audio_reduce model;
			static uint8_t want[MAX_OUT];
			uint32_t want_len;
			uint32_t d;
			uint32_t passes = 0U;
			uint32_t pass_bound;
			uint32_t ratio;

			stream_init();
			if (be)
				g_stream.sample_format =
					SDK_AUDIO_SAMPLE_FORMAT_S16BE;
			CHECK(audio_reduce_request(&g_stream.reduce, tc->hz,
			                           tc->channels) == 0,
			      "request");
			g_out_len = 0U;

			audio_reduce_request(&model, tc->hz, tc->channels);
			audio_reduce_set_input(&model, 44100U, 2U);
			want_len = audio_reduce_run(
				&model, (const int16_t *)g_ref, full_len / 4U,
				(int16_t *)want) * 2U * model.out_channels;
			if (be)
				swap16(want, want_len);

			ratio = model.divisor * 2U / model.out_channels;
			pass_bound = (EXPECTED_PASS_PCM_CAP + FRAME_PCM) /
			             ratio + 4U * model.out_channels;
			for (guard = 0U; guard < 1000U; guard++) {
				d = pass_and_drain();
				CHECK(d <= pass_bound,
				      "case %u pass %u wrote %u bytes (bound %u)",
				      k, passes, d, pass_bound);
				passes++;
				if (d == 0U)
					break;
			}
			CHECK(g_stream.sample_rate == tc->want_hz &&
			      g_stream.channels == tc->want_channels,
			      "case %u reports %u Hz %u ch, want %u/%u", k,
			      g_stream.sample_rate, g_stream.channels,
			      tc->want_hz, tc->want_channels);
			CHECK(g_stream.reduce.in_hz == 44100U &&
			      g_stream.reduce.in_channels == 2U,
			      "case %u decoder format %u/%u", k,
			      g_stream.reduce.in_hz, g_stream.reduce.in_channels);
			CHECK(g_out_len == want_len &&
			      memcmp(g_out, want, want_len) == 0,
			      "case %u be=%u: output %u bytes, model %u, %s", k,
			      be, g_out_len, want_len,
			      g_out_len == want_len ? "differs" : "short");
			CHECK(g_stream.bytes_produced == g_out_len,
			      "case %u bytes_produced %u, ring bytes %u", k,
			      g_stream.bytes_produced, g_out_len);
		}
	}
}

/* audio_stream_reduce_ring(): the in-place pass the native codecs use.
 * The raw PCM sits in a wrapping ring at an odd offset; the result must
 * equal the reducer's one-shot output, also when the work is split over
 * several passes. */
static void test_ring_reduce(void)
{
	enum { CAP = 9000, RAW_FRAMES = 2000 };
	static int16_t raw[RAW_FRAMES * 2];
	static int16_t want[RAW_FRAMES * 2];
	static const struct {
		uint32_t in_ch;
		uint32_t hz;
		uint32_t ch;
	} cases[] = {
		{ 2U, 24000U, 0U }, { 2U, 12000U, 0U }, { 2U, 0U, 1U },
		{ 2U, 24000U, 1U }, { 2U, 12000U, 1U }, { 1U, 0U, 2U },
		{ 1U, 24000U, 2U }, { 1U, 12000U, 2U }, { 1U, 24000U, 0U },
	};
	uint32_t k;
	uint32_t be;

	for (k = 0U; k < sizeof(cases) / sizeof(cases[0]); k++) {
		for (be = 0U; be < 2U; be++) {
			struct SDKAudioStream *s = &g_stream;
			struct audio_reduce model;
			const uint32_t in_ch = cases[k].in_ch;
			uint32_t frames = RAW_FRAMES;
			uint32_t want_frames;
			uint32_t want_bytes;
			uint32_t start;
			uint32_t got;
			uint32_t pass;
			uint32_t done = 0U;
			uint32_t out_bytes = 0U;
			uint32_t i;
			static uint8_t ring_out[CAP];

			memset(s, 0, sizeof(*s));
			s->pcm_capacity = CAP;
			s->pcm_ring_addr = (uintptr_t)g_pcm;
			s->sample_format = be ? SDK_AUDIO_SAMPLE_FORMAT_S16BE :
			                        SDK_AUDIO_SAMPLE_FORMAT_S16LE;
			audio_reduce_request(&s->reduce, cases[k].hz,
			                     cases[k].ch);
			audio_stream_set_format(s, 48000U, in_ch);
			/* Doubling mono data needs room: keep the raw part under
			 * half the ring in that case. */
			if (s->reduce.divisor == 1U && in_ch == 1U)
				frames = 1500U;
			for (i = 0U; i < frames * in_ch; i++)
				raw[i] = (int16_t)(9000.0 * sin(i * 0.071) +
				                   (int32_t)((i * 2654435761U) >> 20) -
				                   2048);

			audio_reduce_request(&model, cases[k].hz, cases[k].ch);
			audio_reduce_set_input(&model, 48000U, in_ch);
			want_frames = audio_reduce_run(&model, raw, frames, want);
			want_bytes = want_frames * 2U * model.out_channels;
			CHECK(s->sample_rate == model.out_hz &&
			      s->channels == model.out_channels,
			      "case %u format %u/%u", k, s->sample_rate,
			      s->channels);

			/* Three passes (the first spans several staging chunks), each
			 * starting where the previous output ended. */
			start = 8000U;
			for (pass = 0U; pass < 3U; pass++) {
				uint32_t n = pass == 0U ? 1300U :
				             pass == 1U ? 1U : frames - done;
				uint8_t *p;
				uint32_t b;

				if (pass == 1U && done + n > frames)
					n = frames - done;
				/* Lay the raw frames into the ring at `start`. */
				b = n * 2U * in_ch;
				p = (uint8_t *)malloc(b);
				memcpy(p, raw + done * in_ch, b);
				if (be)
					swap16(p, b);
				for (i = 0U; i < b; i++)
					g_pcm[(start + i) % CAP] = p[i];
				free(p);
				got = audio_stream_reduce_ring(s, start, b);
				for (i = 0U; i < got; i++)
					ring_out[out_bytes + i] =
						g_pcm[(start + i) % CAP];
				out_bytes += got;
				start = (start + got) % CAP;
				done += n;
			}
			CHECK(done == frames, "case %u consumed %u of %u", k, done,
			      frames);
			if (be)
				swap16(ring_out, out_bytes);
			CHECK(out_bytes == want_bytes &&
			      memcmp(ring_out, want, want_bytes) == 0,
			      "case %u be=%u: ring pass gave %u bytes, model %u, %s",
			      k, be, out_bytes, want_bytes,
			      out_bytes == want_bytes ? "differs" : "length");
		}
	}
}

int main(int argc, char **argv)
{
	const char *fixture = argc > 1 ? argv[1] : "fixtures/tone.mp3";

	load_mp3(fixture);
	test_drain_at_exact_frame_boundary();
	test_pass_cap();
	test_begin_status();
	test_mp3_reduced();
	test_ring_reduce();
	if (failures) {
		printf("audio_stream_decode_test: %d failure(s)\n", failures);
		return 1;
	}
	printf("audio_stream_decode_test: all passed\n");
	return 0;
}
