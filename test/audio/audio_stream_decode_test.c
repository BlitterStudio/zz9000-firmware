/*
 * Behavioural test for the MP3 stream decode pass bound: one decode call
 * must be bounded by the ~1 ms service cap (sdk_audio_codec_io.h) even
 * when the client's high-water budget is the whole PCM ring, and the
 * stream must finish across passes with byte-identical PCM. Drives the
 * production loop in sdk_audio_stream.c -- the exact function
 * sdk_mailbox.c dispatches MP3 streams to -- over a real 128 kbps stereo
 * MP3 (ffmpeg sine, committed under fixtures/).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sdk_audio_stream.h"
#include "sdk_audio_codec_io.h"

#define IN_CAP   (128U * 1024U)
#define PCM_CAP  (256U * 1024U)
#define MAX_OUT  (1024U * 1024U)

/* 1152-sample stereo S16 frame, the production frame_pcm_bytes quantum. */
#define FRAME_PCM (1152U * 2U * 2U)

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

static void test_pass_cap(void)
{
	struct SDKAudioStream *s = &g_stream;
	uint32_t pass_bound = SDK_AUDIO_DECODE_PASS_PCM_BYTES + FRAME_PCM;
	uint32_t ref_len;
	uint32_t total;
	uint32_t passes = 0U;
	uint32_t d;
	uint32_t guard;

	/* Reference: the same production loop, drained every pass, until it
	 * stops making progress (the compressed tail drops below the
	 * lookahead gate without EOF). */
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
	CHECK(passes >= ref_len / SDK_AUDIO_DECODE_PASS_PCM_BYTES,
	      "stream finished in %u extra passes", passes);
}

int main(int argc, char **argv)
{
	const char *fixture = argc > 1 ? argv[1] : "fixtures/tone.mp3";

	load_mp3(fixture);
	test_pass_cap();
	if (failures) {
		printf("audio_stream_decode_test: %d failure(s)\n", failures);
		return 1;
	}
	printf("audio_stream_decode_test: all passed\n");
	return 0;
}
