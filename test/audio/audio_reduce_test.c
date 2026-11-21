/*
 * Behavioural test for the audio stream reducer (audio_reduce.c): divisor
 * selection, decimator frequency response, chunk-split invariance, mono
 * mixdown and duplication, and reconfiguration on a rate change. Every
 * check drives the production audio_reduce_* functions.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_reduce.h"

static int failures;

#define CHECK(cond, ...) do { \
	if (!(cond)) { \
		failures++; \
		printf("FAIL %s:%d: ", __FILE__, __LINE__); \
		printf(__VA_ARGS__); \
		printf("\n"); \
	} \
} while (0)

static struct audio_reduce g_r;

static void configure(uint32_t in_hz, uint32_t in_ch, uint32_t out_hz,
                      uint32_t out_ch)
{
	int rc = audio_reduce_request(&g_r, out_hz, out_ch);

	CHECK(rc == 0, "request(%u, %u) rc %d", out_hz, out_ch, rc);
	audio_reduce_set_input(&g_r, in_hz, in_ch);
}

/* Independent divisor model: the smallest d in {1, 2, 4} with
 * hz / d <= output_hz in real arithmetic, else 4. */
static uint32_t model_divisor(uint32_t hz, uint32_t output_hz)
{
	uint32_t d;

	if (output_hz == 0U)
		return 1U;
	for (d = 1U; d <= 4U; d *= 2U) {
		if ((double)hz / (double)d <= (double)output_hz)
			return d;
	}
	return 4U;
}

static void test_divisor_selection(void)
{
	static const uint32_t rates[] = { 44100U, 48000U, 32000U, 22050U,
	                                  11025U, 8000U, 96000U };
	static const uint32_t limits[] = { 0U, 48000U, 44100U, 32000U, 24000U,
	                                   22050U, 16000U, 12000U, 11025U,
	                                   8000U, 5000U, 1U };
	uint32_t i;
	uint32_t j;

	CHECK(audio_reduce_divisor(44100U, 22050U) == 2U, "44100/22050");
	CHECK(audio_reduce_divisor(48000U, 22050U) == 4U, "48000/22050");
	CHECK(audio_reduce_divisor(48000U, 24000U) == 2U, "48000/24000");
	CHECK(audio_reduce_divisor(48000U, 48000U) == 1U, "48000/48000");
	CHECK(audio_reduce_divisor(48000U, 44100U) == 2U, "48000/44100");
	CHECK(audio_reduce_divisor(32000U, 16000U) == 2U, "32000/16000");
	CHECK(audio_reduce_divisor(32000U, 8000U) == 4U, "32000/8000");
	CHECK(audio_reduce_divisor(32000U, 1000U) == 4U, "none fits -> 4");
	CHECK(audio_reduce_divisor(22050U, 11025U) == 2U, "22050/11025");
	CHECK(audio_reduce_divisor(44100U, 0U) == 1U, "limit 0 = native");
	for (i = 0U; i < sizeof(rates) / sizeof(rates[0]); i++) {
		for (j = 0U; j < sizeof(limits) / sizeof(limits[0]); j++) {
			uint32_t want = model_divisor(rates[i], limits[j]);
			uint32_t got = audio_reduce_divisor(rates[i], limits[j]);

			CHECK(got == want, "rate %u limit %u: divisor %u want %u",
			      rates[i], limits[j], got, want);
		}
	}

	/* Output rate is the stream rate over the divisor, rounded. */
	configure(44100U, 2U, 22050U, 0U);
	CHECK(g_r.divisor == 2U && g_r.out_hz == 22050U &&
	      g_r.out_channels == 2U, "44100 -> %u Hz %u ch", g_r.out_hz,
	      g_r.out_channels);
	configure(48000U, 2U, 22050U, 1U);
	CHECK(g_r.divisor == 4U && g_r.out_hz == 12000U &&
	      g_r.out_channels == 1U, "48000 -> %u Hz %u ch", g_r.out_hz,
	      g_r.out_channels);
	configure(22050U, 1U, 5000U, 0U);
	CHECK(g_r.divisor == 4U && g_r.out_hz == 5513U,
	      "22050/4 rounds to %u", g_r.out_hz);
	configure(32000U, 1U, 0U, 2U);
	CHECK(g_r.divisor == 1U && g_r.out_hz == 32000U &&
	      g_r.out_channels == 2U && audio_reduce_active(&g_r),
	      "channels-only request");
	configure(44100U, 2U, 0U, 0U);
	CHECK(!audio_reduce_active(&g_r), "zero request is passthrough");
	CHECK(audio_reduce_request(&g_r, 0U, 3U) == -1,
	      "three output channels accepted");

	/* Decoder bytes per ring byte, and ring room needed for decoder PCM. */
	configure(48000U, 2U, 24000U, 1U);
	CHECK(audio_reduce_raw_bytes(&g_r, 1000U) == 4000U &&
	      audio_reduce_raw_room(&g_r, 1000U) == 1000U,
	      "stereo d=2 to mono: raw bytes %u room %u",
	      audio_reduce_raw_bytes(&g_r, 1000U),
	      audio_reduce_raw_room(&g_r, 1000U));
	configure(48000U, 1U, 0U, 2U);
	CHECK(audio_reduce_raw_bytes(&g_r, 1000U) == 500U &&
	      audio_reduce_raw_room(&g_r, 1000U) == 500U,
	      "mono d=1 to stereo: raw bytes %u room %u",
	      audio_reduce_raw_bytes(&g_r, 1000U),
	      audio_reduce_raw_room(&g_r, 1000U));
	audio_reduce_request(&g_r, 0U, 2U);
	CHECK(audio_reduce_raw_room(&g_r, 1000U) == 500U,
	      "room before the format is known must allow for duplication");
	audio_reduce_request(&g_r, 12000U, 0U);
	audio_reduce_set_input(&g_r, 48000U, 1U);
	CHECK(audio_reduce_raw_bytes(&g_r, 1000U) == 4000U &&
	      audio_reduce_raw_room(&g_r, 1000U) == 1000U,
	      "mono d=4 raw bytes %u", audio_reduce_raw_bytes(&g_r, 1000U));
}

/* Mono to stereo with decimation: whatever phase the last pass left, the
 * decoder bytes raw_room allows never reduce to more than the free room. */
static void test_raw_room_bounds_output(void)
{
	static const uint32_t divisors[] = { 2U, 4U };
	static int16_t in[64];
	static int16_t out[2U * 64U];
	uint32_t di;
	uint32_t i;

	for (i = 0U; i < 64U; i++)
		in[i] = (int16_t)(i * 97U);
	for (di = 0U; di < 2U; di++) {
		uint32_t d = divisors[di];
		uint32_t lead;

		for (lead = 0U; lead < 2U * d; lead++) {
			uint32_t free_bytes;

			for (free_bytes = 0U; free_bytes <= 40U; free_bytes += 2U) {
				uint32_t room;
				uint32_t produced;

				configure(48000U, 1U, 48000U / d, 2U);
				(void)audio_reduce_run(&g_r, in, lead, out);
				room = audio_reduce_raw_room(&g_r, free_bytes);
				CHECK((room & 1U) == 0U, "d=%u room %u is odd", d,
				      room);
				produced = audio_reduce_run(&g_r, in, room / 2U, out);
				CHECK(produced * 4U <= free_bytes,
				      "d=%u lead %u free %u: room %u gives %u bytes",
				      d, lead, free_bytes, room, produced * 4U);
			}
		}
	}
}

/* Amplitude of the output for a sine of the given amplitude and frequency
 * (cycles per input sample), as an RMS over the settled output. */
static double tone_gain_db(uint32_t in_hz, uint32_t in_ch, uint32_t out_hz,
                           uint32_t out_ch, double freq)
{
	enum { IN_FRAMES = 40000, SKIP = 500 };
	const double amp = 20000.0;
	static int16_t in[IN_FRAMES * 2];
	static int16_t out[IN_FRAMES * 2];
	uint32_t frames;
	uint32_t i;
	uint32_t c;
	double acc = 0.0;
	uint32_t n = 0U;

	configure(in_hz, in_ch, out_hz, out_ch);
	for (i = 0U; i < IN_FRAMES; i++) {
		int16_t v = (int16_t)lrint(amp * sin(2.0 * M_PI * freq * i));

		for (c = 0U; c < in_ch; c++)
			in[in_ch * i + c] = v;
	}
	frames = audio_reduce_run(&g_r, in, IN_FRAMES, out);
	for (i = SKIP; i < frames; i++) {
		double v = out[g_r.out_channels * i];

		acc += v * v;
		n++;
	}
	return 20.0 * log10(sqrt(acc / n) * sqrt(2.0) / amp + 1e-12);
}

static void test_frequency_response(void)
{
	static const uint32_t divisors[] = { 2U, 4U };
	uint32_t k;

	for (k = 0U; k < 2U; k++) {
		const uint32_t d = divisors[k];
		const uint32_t in_hz = 48000U;
		const uint32_t limit = in_hz / d;
		/* Passband edge 0.40 of the output rate, in cycles per input
		 * sample. */
		const double pass_edge = 0.40 / d;
		double worst_pass = 0.0;
		double worst_stop = -1000.0;
		double f;
		double at_1k;
		double worst_f = 0.0;

		for (f = 0.005; f <= pass_edge + 1e-9; f += 0.005 / d) {
			double g = tone_gain_db(in_hz, 2U, limit, 0U, f);

			if (fabs(g) > fabs(worst_pass)) {
				worst_pass = g;
				worst_f = f;
			}
		}
		CHECK(fabs(worst_pass) <= 0.1,
		      "d=%u passband deviation %.4f dB at %.4f fs", d,
		      worst_pass, worst_f);
		at_1k = tone_gain_db(in_hz, 2U, limit, 0U, 1000.0 / in_hz);
		CHECK(fabs(at_1k) <= 0.1, "d=%u 1 kHz gain %.4f dB", d, at_1k);

		/* Input frequencies at or above 0.60 of the output rate that
		 * fold onto the 0..0.40 passband of the output. */
		for (f = 0.6 / d; f < 0.5; f += 0.0007) {
			double fo = f * d;
			double g;

			if (fabs(fo - floor(fo + 0.5)) > 0.4)
				continue;
			g = tone_gain_db(in_hz, 2U, limit, 0U, f);
			if (g > worst_stop) {
				worst_stop = g;
				worst_f = f;
			}
		}
		printf("d=%u: passband max deviation %.5f dB (up to 0.40 x "
		       "output rate), 1 kHz %.5f dB, alias rejection %.1f dB "
		       "(worst at %.4f x input rate)\n",
		       d, worst_pass, at_1k, -worst_stop, worst_f);
		CHECK(-worst_stop >= 70.0,
		      "d=%u alias rejection only %.1f dB", d, -worst_stop);
	}
}

/* Plain double-precision model of one decimate-by-2 stage built from the
 * same taps, used to check the production indexing and phase. */
static void model_stage(const int16_t *x, uint32_t n, int16_t *y,
                        uint32_t *produced)
{
	double h[AUDIO_REDUCE_TAPS];
	uint32_t centre = (AUDIO_REDUCE_TAPS - 1U) / 2U;
	uint32_t i;
	uint32_t p;
	uint32_t count = 0U;

	memset(h, 0, sizeof(h));
	h[centre] = (double)AUDIO_REDUCE_CENTRE / (double)(1U << 30);
	for (i = 0U; i < AUDIO_REDUCE_SIDES; i++) {
		double v = (double)audio_reduce_side[i] / (double)(1U << 30);

		h[centre - (2U * i + 1U)] = v;
		h[centre + (2U * i + 1U)] = v;
	}
	for (p = 1U; p < n; p += 2U) {
		double acc = 0.0;
		double r;

		for (i = 0U; i < AUDIO_REDUCE_TAPS; i++) {
			int64_t idx = (int64_t)p - (AUDIO_REDUCE_TAPS - 1U) + i;

			if (idx >= 0)
				acc += h[i] * x[idx];
		}
		r = floor(acc + 0.5);
		if (r > 32767.0)
			r = 32767.0;
		if (r < -32768.0)
			r = -32768.0;
		y[count++] = (int16_t)r;
	}
	*produced = count;
}

static void test_matches_model(void)
{
	enum { N = 3001 };
	static int16_t x[N];
	static int16_t out[N];
	static int16_t ref[N];
	static int16_t ref2[N];
	uint32_t ref_n;
	uint32_t ref2_n;
	uint32_t got;
	uint32_t i;
	uint32_t seed = 12345U;
	int bad = 0;

	for (i = 0U; i < N; i++) {
		seed = seed * 1664525U + 1013904223U;
		/* Loud noise so the saturating clip path is exercised. */
		x[i] = (int16_t)((int32_t)(seed >> 16) - 32768);
	}
	model_stage(x, N, ref, &ref_n);
	configure(48000U, 1U, 24000U, 0U);
	got = audio_reduce_run(&g_r, x, N, out);
	CHECK(got == ref_n, "d=2 produced %u model %u", got, ref_n);
	for (i = 0U; i < got && i < ref_n; i++) {
		if (abs((int)out[i] - (int)ref[i]) > 1)
			bad++;
	}
	CHECK(bad == 0, "d=2 differs from the double model in %d samples", bad);

	/* Two stages: the model's second stage reads the first's output. */
	model_stage(ref, ref_n, ref2, &ref2_n);
	configure(48000U, 1U, 12000U, 0U);
	got = audio_reduce_run(&g_r, x, N, out);
	CHECK(got == ref2_n, "d=4 produced %u model %u", got, ref2_n);
	bad = 0;
	for (i = 0U; i < got && i < ref2_n; i++) {
		/* The model feeds stage 2 the first stage's rounded output,
		 * exactly like the production code; allow one LSB. */
		if (abs((int)out[i] - (int)ref2[i]) > 1)
			bad++;
	}
	CHECK(bad == 0, "d=4 differs from the double model in %d samples", bad);
}

struct split_case {
	uint32_t in_ch;
	uint32_t out_hz_limit;
	uint32_t out_ch;
};

static void test_chunk_split_invariance(void)
{
	static const struct split_case cases[] = {
		{ 2U, 24000U, 0U }, { 2U, 12000U, 0U }, { 2U, 0U, 1U },
		{ 2U, 24000U, 1U }, { 2U, 12000U, 1U }, { 1U, 0U, 2U },
		{ 1U, 24000U, 2U }, { 1U, 12000U, 2U }, { 1U, 24000U, 0U },
		{ 1U, 12000U, 0U },
	};
	enum { N = 6007 };
	static int16_t in[N * 2];
	static int16_t whole[N * 2];
	static int16_t split[N * 2];
	uint32_t k;

	for (k = 0U; k < sizeof(cases) / sizeof(cases[0]); k++) {
		const struct split_case *tc = &cases[k];
		uint32_t seed = 99U + k;
		uint32_t whole_n;
		uint32_t split_n = 0U;
		uint32_t pos = 0U;
		uint32_t i;
		struct audio_reduce saved;

		for (i = 0U; i < N * tc->in_ch; i++) {
			seed = seed * 1664525U + 1013904223U;
			in[i] = (int16_t)(8000.0 * sin(i * 0.0137) +
			                  (int32_t)((seed >> 16) % 4000U) - 2000);
		}
		configure(48000U, tc->in_ch, tc->out_hz_limit, tc->out_ch);
		saved = g_r;
		whole_n = audio_reduce_run(&g_r, in, N, whole);

		g_r = saved;
		seed = 7U * (k + 1U);
		while (pos < N) {
			uint32_t n;

			seed = seed * 1664525U + 1013904223U;
			/* Mostly odd sizes, sometimes 1 and sometimes large. */
			n = 1U + ((seed >> 16) % 37U);
			if ((seed >> 8) % 11U == 0U)
				n = 1U;
			if (pos + n > N)
				n = N - pos;
			split_n += audio_reduce_run(&g_r, in + pos * tc->in_ch, n,
			                            split + split_n * g_r.out_channels);
			pos += n;
		}
		CHECK(split_n == whole_n, "case %u: split %u frames, whole %u",
		      k, split_n, whole_n);
		CHECK(memcmp(whole, split,
		             (size_t)whole_n * g_r.out_channels * 2U) == 0,
		      "case %u: split output differs", k);
	}
}

static void test_mixdown_and_duplication(void)
{
	static const int16_t stereo[] = {
		1000, 2001,   /* 1500.5 rounds half up to 1501 */
		-3, 0,        /* -1.5 -> -1 */
		-1, -2,       /* -1.5 -> -1 */
		32767, 32767, /* no overflow */
		-32768, -32768,
		-32768, 32767, /* -0.5 -> 0 */
		5, 7,
		-5, -7,
	};
	static const int16_t want[] = { 1501, -1, -1, 32767, -32768, 0, 6, -6 };
	int16_t out[16];
	uint32_t n;
	uint32_t i;

	configure(44100U, 2U, 0U, 1U);
	n = audio_reduce_run(&g_r, stereo, 8U, out);
	CHECK(n == 8U, "mixdown produced %u frames", n);
	for (i = 0U; i < 8U && i < n; i++)
		CHECK(out[i] == want[i], "mix %u: %d want %d", i, out[i],
		      want[i]);

	{
		static const int16_t mono[] = { 1, -2, 32767, -32768, 0 };
		int16_t dup[10];

		configure(44100U, 1U, 0U, 2U);
		n = audio_reduce_run(&g_r, mono, 5U, dup);
		CHECK(n == 5U, "duplication produced %u frames", n);
		for (i = 0U; i < 5U; i++)
			CHECK(dup[2U * i] == mono[i] && dup[2U * i + 1U] == mono[i],
			      "dup %u: %d/%d want %d", i, dup[2U * i],
			      dup[2U * i + 1U], mono[i]);
	}

	/* A steady level passes the decimator with unity gain exactly (the
	 * taps sum to 2^30), in either channel layout. */
	{
		enum { N = 400 };
		static int16_t dc_in[N * 2];
		static int16_t dc_out[N * 2];
		uint32_t frames;

		for (i = 0U; i < N; i++) {
			dc_in[2U * i] = 1000;
			dc_in[2U * i + 1U] = 2000;
		}
		configure(48000U, 2U, 24000U, 1U);
		frames = audio_reduce_run(&g_r, dc_in, N, dc_out);
		CHECK(frames == N / 2U, "dc mixdown frames %u", frames);
		CHECK(dc_out[frames - 1U] == 1500, "dc mixdown level %d",
		      dc_out[frames - 1U]);

		for (i = 0U; i < N; i++)
			dc_in[i] = -777;
		configure(48000U, 1U, 12000U, 2U);
		frames = audio_reduce_run(&g_r, dc_in, N, dc_out);
		CHECK(frames == N / 4U, "dc dup frames %u", frames);
		CHECK(dc_out[2U * (frames - 1U)] == -777 &&
		      dc_out[2U * (frames - 1U) + 1U] == -777,
		      "dc dup level %d/%d", dc_out[2U * (frames - 1U)],
		      dc_out[2U * (frames - 1U) + 1U]);
	}
}

static void test_rate_change_resets(void)
{
	enum { N = 300 };
	static int16_t in[N * 2];
	static int16_t a[N * 2];
	static int16_t b[N * 2];
	uint32_t na;
	uint32_t nb;
	uint32_t i;
	struct audio_reduce fresh;

	for (i = 0U; i < N * 2U; i++)
		in[i] = (int16_t)(12000.0 * sin(i * 0.31));

	audio_reduce_request(&g_r, 24000U, 0U);
	CHECK(audio_reduce_set_input(&g_r, 48000U, 2U) == 1, "first input");
	CHECK(g_r.divisor == 2U, "divisor %u", g_r.divisor);
	CHECK(audio_reduce_set_input(&g_r, 48000U, 2U) == 0,
	      "same input reconfigured");
	(void)audio_reduce_run(&g_r, in, N, a);

	/* 24 kHz fits the limit: divisor drops to 1 and nothing is reduced. */
	CHECK(audio_reduce_set_input(&g_r, 24000U, 2U) == 1, "rate change");
	CHECK(g_r.divisor == 1U && g_r.out_hz == 24000U &&
	      !audio_reduce_active(&g_r), "24 kHz divisor %u out %u",
	      g_r.divisor, g_r.out_hz);

	/* 44.1 kHz needs 2 again, from a clean filter. */
	CHECK(audio_reduce_set_input(&g_r, 44100U, 2U) == 1, "back up");
	CHECK(g_r.divisor == 2U && g_r.out_hz == 22050U,
	      "44100 divisor %u out %u", g_r.divisor, g_r.out_hz);
	na = audio_reduce_run(&g_r, in, N, a);

	audio_reduce_request(&fresh, 24000U, 0U);
	audio_reduce_set_input(&fresh, 44100U, 2U);
	nb = audio_reduce_run(&fresh, in, N, b);
	CHECK(na == nb && memcmp(a, b, (size_t)na * 4U) == 0,
	      "output after a rate change depends on earlier input");
}

int main(void)
{
	test_divisor_selection();
	test_raw_room_bounds_output();
	test_frequency_response();
	test_matches_model();
	test_chunk_split_invariance();
	test_mixdown_and_duplication();
	test_rate_change_resets();
	if (failures) {
		printf("audio_reduce_test: %d failure(s)\n", failures);
		return 1;
	}
	printf("audio_reduce_test: all passed\n");
	return 0;
}
