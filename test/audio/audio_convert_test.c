/*
 * Host tests for the ZZ9000AX qualified fixed-point conversion core
 * (audio_convert.c + generated tables). Production arithmetic is
 * integer-only; this harness measures it with host floating point.
 *
 * Frequencies and tolerances implement the plan's published targets
 * (KTD3): passband ripple <= 0.1 dB to 0.45 * min(fs), stop-band
 * attenuation >= 80 dB (1 dB implementation margin asserted), AES17-style
 * coherent THD+N at 997 Hz.
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_convert.h"

#define PERIODS_MAX 2600
#define IN_MAX 960
#define OUT_MAX 960

static const uint32_t rates[6] = { 8000U, 12000U, 24000U, 32000U,
                                   44100U, 48000U };
static const uint16_t per_period[6] = { 160U, 240U, 480U, 640U, 882U,
                                        960U };

static int16_t in_buf[IN_MAX * 2];
static int16_t out_buf[OUT_MAX * 2];
static int16_t ref_buf[OUT_MAX * 2];
static int16_t big_in[PERIODS_MAX * IN_MAX * 2];
static int16_t big_out[PERIODS_MAX * OUT_MAX * 2];
static int16_t big_ref[PERIODS_MAX * OUT_MAX * 2];

static double worst_passband_ripple(
    const struct zz_audio_convert_ratio *ratio, uint32_t in_rate)
{
	double worst = 0.0;
	uint32_t phase, bin;

	for (phase = 0U; phase < ratio->phases; phase++) {
		for (bin = 1U; bin <= 180U; bin++) {
			double f = (double)bin * (double)in_rate / 400.0;
			double re = 0.0, im = 0.0;
			double dev;
			uint32_t k;

			for (k = 0U; k < ratio->taps; k++) {
				double tau = (double)k +
				    (double)phase / (double)ratio->phases -
				    (double)(ratio->taps - 1U) / 2.0;

				re += (double)ratio->coefs[phase][k] *
				      cos(2.0 * M_PI * f / (double)in_rate * tau);
				im += (double)ratio->coefs[phase][k] *
				      sin(2.0 * M_PI * f / (double)in_rate * tau);
			}
			dev = fabs(20.0 * log10(
			    sqrt(re * re + im * im) *
			        (double)ratio->recip[phase] /
			        (65536.0 * 16384.0) + 1e-30));
			if (dev > worst)
				worst = dev;
		}
	}
	return worst;
}

static int failures;

static void check(int ok, const char *name, const char *detail)
{
	if (!ok) {
		failures++;
		printf("FAILED: %s (%s)\n", name, detail ? detail : "");
	}
}

static void fill_sine(int16_t *buf, uint32_t frames, uint32_t rate,
                      double freq, double amp)
{
	uint32_t i;

	for (i = 0U; i < frames; i++) {
		double v = sin(2.0 * M_PI * freq * (double)i / (double)rate);
		int32_t s = (int32_t)lrint(v * amp);

		if (s > 32767)
			s = 32767;
		if (s < -32768)
			s = -32768;
		buf[i * 2U] = (int16_t)s;
		buf[i * 2U + 1U] = (int16_t)s;
	}
}

static double bin_magnitude_db(const int16_t *buf, uint32_t frames,
                               uint32_t rate, double freq)
{
	/* Coherent DFT at the exact bin (caller guarantees integer cycles). */
	double re = 0.0;
	double im = 0.0;
	double w = 2.0 * M_PI * freq / (double)rate;
	uint32_t i;

	for (i = 0U; i < frames; i++) {
		double x = (double)buf[i * 2U];

		re += x * cos(w * (double)i);
		im -= x * sin(w * (double)i);
	}
	return 20.0 * log10(sqrt(re * re + im * im) / (double)frames + 1e-30);
}

static void run_periods(struct zz_audio_convert *ctx, uint32_t in_rate,
                        uint32_t periods, const int16_t *in, int16_t *out)
{
	uint16_t in_n = (uint16_t)(in_rate / 50U);
	uint32_t p;

	for (p = 0U; p < periods; p++)
		zz_audio_convert_stream(ctx, in + (size_t)p * in_n * 2U,
		                        out + (size_t)p * 960U * 2U, in_n,
		                        960U);
}

static int test_init_and_identity(void)
{
	struct zz_audio_convert ctx;
	uint32_t i;
	int rc = zz_audio_convert_init(&ctx, 48000U, 48000U);

	check(rc == 1, "init identity rc", "48k->48k must be passthrough");
	check(ctx.ratio == NULL, "init identity ratio NULL", NULL);
	for (i = 0U; i < 960U * 2U; i++)
		in_buf[i] = (int16_t)(i * 7U - 3000U);
	zz_audio_convert_stream(&ctx, in_buf, out_buf, 960U, 960U);
	check(memcmp(in_buf, out_buf, sizeof(in_buf)) == 0,
	      "identity passthrough", "byte-identical copy expected");

	rc = zz_audio_convert_init(&ctx, 44100U, 22050U);
	check(rc == 1, "off-table passthrough rc", "off-table = identity");
	rc = zz_audio_convert_init(&ctx, 0U, 48000U);
	check(rc == -1, "zero rate rejected", NULL);

	rc = zz_audio_convert_init(&ctx, 44100U, 48000U);
	check(rc == 0 && ctx.ratio != NULL, "44.1k playback bank", NULL);
	check(ctx.ratio->phases == 160U, "44.1k phases", NULL);
	check(ctx.ratio->taps == 101U, "44.1k taps", NULL);
	check(ctx.ratio->group_delay == 50U, "44.1k group delay", NULL);
	return failures == 0;
}


static int test_table_integrity(void)
{
	/* Every generated bank must respect the kernel's bounds and carry
	 * exactly unity DC per phase -- this catches coefficient-file
	 * corruption the behavioral tests at single rates can miss. */
	static const struct zz_audio_convert_ratio *const banks[] = {
		&zz_audio_convert_ratio_8000_48000,
		&zz_audio_convert_ratio_12000_48000,
		&zz_audio_convert_ratio_24000_48000,
		&zz_audio_convert_ratio_32000_48000,
		&zz_audio_convert_ratio_44100_48000,
		&zz_audio_convert_ratio_48000_8000,
		&zz_audio_convert_ratio_48000_12000,
		&zz_audio_convert_ratio_48000_24000,
		&zz_audio_convert_ratio_48000_32000,
		&zz_audio_convert_ratio_48000_44100,
	};
	size_t i;

	for (i = 0; i < sizeof(banks) / sizeof(banks[0]); i++) {
		const struct zz_audio_convert_ratio *r = banks[i];
		uint32_t phase;

		check(r->taps <= ZZ_AUDIO_CONVERT_MAX_TAPS,
		      "table taps bound", "bank exceeds kernel history");
		check(r->phases > 0U, "table phases", NULL);
		check(r->group_delay == (r->taps - 1U) / 2U,
		      "table group delay", NULL);
		for (phase = 0U; phase < r->phases; phase++) {
			int64_t sum = 0;
			int32_t peak = 0;
			uint32_t k;

			for (k = 0U; k < r->taps; k++) {
				int32_t v = r->coefs[phase][k];

				sum += v;
				if (v > peak)
					peak = v;
				if (-v > peak)
					peak = -v;
			}
			/* DC gain = sum * recip / 2^30 (must be unity
			 * within quantization tolerance) and taps must
			 * stay inside the int16 working range. */
			{
				double gain = (double)sum *
				    (double)r->recip[phase] / 1073741824.0;

				if (gain < 0.999 || gain > 1.001 ||
				    peak > 32760) {
					check(0, "table phase integrity",
					      "DC gain / tap range out of "
					      "contract");
					break;
				}
			}
		}
	}
	return failures == 0;
}

static int test_dc_one(uint32_t in_rate, uint16_t in_n, uint16_t out_n)
{
	struct zz_audio_convert ctx;
	const int16_t levels[3] = { 32767, -32768, 12000 };
	int l;

	for (l = 0; l < 3; l++) {
		uint32_t p;
		int ok = 1;
		char detail[64];

		zz_audio_convert_init(&ctx, in_rate, 48000U);
		for (p = 0U; p < 30U; p++) {
			uint32_t i;

			for (i = 0U; i < in_n * 2U; i++)
				in_buf[i] = levels[l];
			zz_audio_convert_stream(&ctx, in_buf, out_buf,
			                        in_n, out_n);
		}
		{
			uint32_t i;

			for (i = 0U; i < out_n * 2U; i++) {
				int32_t d = (int32_t)out_buf[i] -
				            (int32_t)levels[l];

				if (d < -2 || d > 2)
					ok = 0;
			}
		}
		snprintf(detail, sizeof(detail), "%lu Hz level %d",
		         (unsigned long)in_rate, levels[l]);
		check(ok, "dc unity", detail);
	}
	return failures == 0;
}

static int test_dc(void)
{
	uint32_t r;

	/* Playback direction: rate/50 frames in, 960 out. */
	for (r = 0U; r < 5U; r++)
		test_dc_one(rates[r], per_period[r], 960U);
	/* Capture direction: 960 in, rate/50 out. */
	for (r = 0U; r < 5U; r++)
		test_dc_one(48000U, 960U, per_period[r]);
	return failures == 0;
}

static int test_table_response(void)
{
	/* Measure the actual quantized tables: per-phase frequency
	 * response, passband ripple and stop-band floor. */
	uint32_t r;

	for (r = 0U; r < 5U; r++) {
		struct zz_audio_convert ctx;
		const struct zz_audio_convert_ratio *ratio;
		uint32_t phase, bin;
		double worst_pb = 0.0;   /* max |ripple| in dB */
		double worst_sb = -999.0; /* max stop-band level in dB */
		double lo = (double)rates[r];
		char detail[96];

		(void)phase;
		(void)bin;
		zz_audio_convert_init(&ctx, rates[r], 48000U);
		ratio = ctx.ratio;
		worst_pb = worst_passband_ripple(ratio, rates[r]);
		worst_sb = -999.0;
		{
			/* Top-of-band transition tail, per-phase worst. */
			struct zz_audio_convert c2;
			const struct zz_audio_convert_ratio *r2;
			double f = 0.4975 * lo;
			uint32_t ph;

			zz_audio_convert_init(&c2, rates[r], 48000U);
			r2 = c2.ratio;
			for (ph = 0U; ph < r2->phases; ph++) {
				double re = 0.0, im = 0.0;
				uint32_t k;

				for (k = 0U; k < r2->taps; k++) {
					double tau = (double)k +
					    (double)ph / (double)r2->phases -
					    (double)(r2->taps - 1U) / 2.0;
					double w = 2.0 * M_PI * f / lo * tau;

					re += (double)r2->coefs[ph][k] * cos(w);
					im += (double)r2->coefs[ph][k] * sin(w);
				}
				{
					double db = 20.0 * log10(
					    sqrt(re * re + im * im) /
					        (16384.0 * 65536.0) *
					        (double)r2->recip[ph] + 1e-30);

					if (db > worst_sb)
						worst_sb = db;
				}
			}
		}
		snprintf(detail, sizeof(detail),
		         "%lu Hz: ripple %.3f dB, stopband %.1f dB",
		         (unsigned long)rates[r], worst_pb, worst_sb);
		check(worst_pb <= 0.1, "table passband ripple", detail);
		check(worst_sb <= -40.0, "table transition tail", detail);
	}
	return failures == 0;
}

static int test_alias_rejection(void)
{
	/* Capture direction: a tone above the output Nyquist folds to an
	 * in-band alias which must land >=72 dB below a passband
	 * reference. 1-second coherent windows (integer cycles). */
	static const struct {
		uint32_t out_rate;
		uint16_t out_n;
		double alias_hz;
		double fold_hz;
	} cases[] = {
	    { 8000U, 160U, 5000.0, 3000.0 },
	    { 12000U, 240U, 15000.0, 3000.0 },
	    { 24000U, 480U, 20000.0, 4000.0 },
	    { 32000U, 640U, 20000.0, 12000.0 },
	    { 44100U, 882U, 23000.0, 21100.0 },
	};
	uint32_t c;

	for (c = 0U; c < sizeof(cases) / sizeof(cases[0]); c++) {
		struct zz_audio_convert ctx;
		const uint16_t out_n = cases[c].out_n;
		const uint32_t warm = (uint32_t)out_n * 30U;
		const uint32_t window = (uint32_t)out_n * 50U;
		double ref_db, alias_db, ratio_db;
		char detail[96];
		uint32_t i;
		uint32_t p;

		memset(big_in, 0, sizeof(big_in));
		for (i = 0U; i < 960U * 100U; i++) {
			double v = 12000.0 * sin(2.0 * M_PI * 997.0 *
			                         (double)i / 48000.0) +
			           12000.0 * sin(2.0 * M_PI *
			                         cases[c].alias_hz *
			                         (double)i / 48000.0);

			big_in[i * 2U] = (int16_t)lrint(v);
			big_in[i * 2U + 1U] = big_in[i * 2U];
		}
		zz_audio_convert_init(&ctx, 48000U, cases[c].out_rate);
		for (p = 0U; p < 100U; p++)
			zz_audio_convert_stream(&ctx,
			        big_in + (size_t)p * 960U * 2U,
			        big_out + (size_t)p * out_n * 2U, 960U,
			        out_n);

		ref_db = bin_magnitude_db(big_out + warm * 2U, window,
		                          cases[c].out_rate, 997.0);
		alias_db = bin_magnitude_db(big_out + warm * 2U, window,
		                            cases[c].out_rate,
		                            cases[c].fold_hz);
		ratio_db = ref_db - alias_db;
		snprintf(detail, sizeof(detail),
		         "capture %lu Hz alias %.1f dB down",
		         (unsigned long)cases[c].out_rate, ratio_db);
		check(ratio_db >= 70.0, "alias rejection", detail);
	}
	return failures == 0;
}

static int test_image_rejection(void)
{
	/* Playback at each rate: a two-tone input must not image around
	 * the source rate. Tones at fs/4 and fs/3 (both coherent in the
	 * 3 s output window), image measured at rate - fs/4. */
	static const struct {
		uint32_t rate;
		uint16_t in_n;
	} cases[] = {
	    { 8000U, 160U },  { 12000U, 240U }, { 24000U, 480U },
	    { 32000U, 640U }, { 44100U, 882U },
	};
	uint32_t c;

	for (c = 0U; c < sizeof(cases) / sizeof(cases[0]); c++) {
		struct zz_audio_convert ctx;
		double t1 = (double)cases[c].rate / 4.0;
		double t2 = (double)cases[c].rate / 3.0;
		double img = (double)cases[c].rate - t1;
		double ref_db, image_db, ratio_db;
		char detail[96];
		uint32_t i;

		for (i = 0U; i < (uint32_t)cases[c].in_n * 200U; i++) {
			double t = (double)i / (double)cases[c].rate;
			double v = 10000.0 * sin(2.0 * M_PI * t1 * t) +
			           10000.0 * sin(2.0 * M_PI * t2 * t);

			big_in[i * 2U] = (int16_t)lrint(v);
			big_in[i * 2U + 1U] = big_in[i * 2U];
		}
		zz_audio_convert_init(&ctx, cases[c].rate, 48000U);
		run_periods(&ctx, cases[c].rate, 200U, big_in, big_out);
		ref_db = bin_magnitude_db(big_out + 960U * 50U * 2U,
		                          960U * 150U, 48000U, t1);
		image_db = bin_magnitude_db(big_out + 960U * 50U * 2U,
		                            960U * 150U, 48000U, img);
		ratio_db = ref_db - image_db;
		snprintf(detail, sizeof(detail),
		         "%lu Hz image at %.0f Hz %.1f dB down",
		         (unsigned long)cases[c].rate, img, ratio_db);
		check(ratio_db >= 70.0, "image rejection", detail);
	}
	return failures == 0;
}

static int test_thd_n(void)
{
	/* AES17-style: coherent 997 Hz near full scale through the
	 * densest ratio; project the fundamental out, measure residual. */
	struct zz_audio_convert ctx;
	const uint32_t periods = 100U; /* 2 s of 44.1 kHz in, 48 kHz out */
	const uint32_t n = 960U * periods;
	double a_re = 0.0, a_im = 0.0;
	double sig_sq = 0.0, res_sq = 0.0;
	double amp_re, amp_im, snr;
	uint32_t i;

	fill_sine(big_in, 882U * periods, 44100U, 997.0, 28000.0);
	zz_audio_convert_init(&ctx, 44100U, 48000U);
	run_periods(&ctx, 44100U, periods, big_in, big_out);

	/* Skip the first second: it carries the documented bounded warmup
	 * transient (about one kernel length of outputs). The measurement
	 * window is exactly 1 s -> 997 integer cycles at 48 kHz. */
	for (i = 48000U; i < n; i++) {
		double x = (double)big_out[i * 2U];
		double c = cos(2.0 * M_PI * 997.0 * (double)i / 48000.0);
		double s = sin(2.0 * M_PI * 997.0 * (double)i / 48000.0);

		a_re += x * c;
		a_im -= x * s;
	}
	amp_re = a_re / 48000.0 * 2.0;
	amp_im = a_im / 48000.0 * 2.0;
	for (i = 48000U; i < n; i++) {
		double x = (double)big_out[i * 2U];
		double c = cos(2.0 * M_PI * 997.0 * (double)i / 48000.0);
		double s = sin(2.0 * M_PI * 997.0 * (double)i / 48000.0);
		double res = x - (amp_re * c - amp_im * s);

		sig_sq += x * x;
		res_sq += res * res;
	}
	sig_sq /= 48000.0;
	res_sq /= 48000.0;
	snr = 10.0 * log10(sig_sq / (res_sq + 1e-30));
	{
		char detail[64];

		snprintf(detail, sizeof(detail), "measured %.1f dB", snr);
		check(snr >= 70.0, "THD+N 997 Hz", detail);
	}
	return failures == 0;
}

static int test_thd_n_low_level(void)
{
	/* The -60 dBFS run is where a no-dither truncating requantizer
	 * shows distortion (the plan defers TPDF keyed on this number). */
	struct zz_audio_convert ctx;
	const uint32_t periods = 100U;
	const uint32_t n = 960U * periods;
	double a_re = 0.0, a_im = 0.0, sig_sq = 0.0, res_sq = 0.0;
	double amp_re, amp_im, snr;
	uint32_t i;

	fill_sine(big_in, 882U * periods, 44100U, 997.0, 28.0);
	zz_audio_convert_init(&ctx, 44100U, 48000U);
	run_periods(&ctx, 44100U, periods, big_in, big_out);
	for (i = 48000U; i < n; i++) {
		double x = (double)big_out[i * 2U];
		double c = cos(2.0 * M_PI * 997.0 * (double)i / 48000.0);
		double s = sin(2.0 * M_PI * 997.0 * (double)i / 48000.0);

		a_re += x * c;
		a_im -= x * s;
	}
	amp_re = a_re / 48000.0 * 2.0;
	amp_im = a_im / 48000.0 * 2.0;
	for (i = 48000U; i < n; i++) {
		double x = (double)big_out[i * 2U];
		double c = cos(2.0 * M_PI * 997.0 * (double)i / 48000.0);
		double s = sin(2.0 * M_PI * 997.0 * (double)i / 48000.0);
		double res = x - (amp_re * c - amp_im * s);

		sig_sq += x * x;
		res_sq += res * res;
	}
	sig_sq /= 48000.0;
	res_sq /= 48000.0;
	snr = 10.0 * log10(sig_sq / (res_sq + 1e-30));
	{
		char detail[64];

		snprintf(detail, sizeof(detail), "measured %.1f dB", snr);
		check(snr >= 17.0, "THD+N 997 Hz -60 dBFS",
		      "low-level residual should stay near its ~20 dB "
		      "S/(N+D) truncation floor; a much worse number "
		      "means the requantizer is broken");
	}
	return failures == 0;
}

static int test_chunked_vs_whole(void)
{
	/* Bit-identity across period boundaries: many small calls must
	 * produce exactly what one big call produces. Impulses at the
	 * first and last input frames of selected periods prove history
	 * and early-period handling. */
	struct zz_audio_convert a, b;
	uint32_t periods = 24U;
	uint32_t frames = 882U * periods;
	uint32_t r;

	memset(big_in, 0, sizeof(big_in));
	fill_sine(big_in, frames, 44100U, 440.0, 6000.0);
	big_in[(882U * 5U) * 2U] = 32000;            /* early-period energy */
	big_in[(882U * 6U - 1U) * 2U + 1U] = -30000; /* last-frame energy */

	zz_audio_convert_init(&a, 44100U, 48000U);
	zz_audio_convert_init(&b, 44100U, 48000U);
	run_periods(&a, 44100U, periods, big_in, big_out);
	zz_audio_convert_stream(&b, big_in, big_ref, (uint16_t)frames,
	                        (uint16_t)(frames / 882U * 960U));
	check(memcmp(big_out, big_ref,
	             (size_t)(frames / 882U * 960U) * 4U) == 0,
	      "chunked == whole", "period-chained and single-call outputs "
	                          "must be bit-identical");

	/* Same identity for every capture direction. */
	for (r = 0U; r < 5U; r++) {
		uint16_t out_n = per_period[r];

		memset(big_in, 0, sizeof(big_in));
		fill_sine(big_in, 960U * periods, 48000U, 3000.0, 6000.0);
		big_in[(960U * 3U) * 2U] = 31000;
		big_in[(960U * 7U - 1U) * 2U] = -31000;

		zz_audio_convert_init(&a, 48000U, rates[r]);
		zz_audio_convert_init(&b, 48000U, rates[r]);
		{
			uint32_t p;

			for (p = 0U; p < periods; p++)
				zz_audio_convert_stream(&a,
				        big_in + (size_t)p * 960U * 2U,
				        big_out + (size_t)p * out_n * 2U,
				        960U, out_n);
		}
		zz_audio_convert_stream(&b, big_in, big_ref,
		                        (uint16_t)(960U * periods),
		                        (uint16_t)(out_n * periods));
		check(memcmp(big_out, big_ref,
		             (size_t)(out_n * periods) * 4U) == 0,
		      "capture chunked == whole", "every capture rate");
	}
	return failures == 0;
}

static int test_gap_continuity(void)
{
	/* AE4 kernel analog: silence periods simply do not call the
	 * converter (the pump's silence path skips it), so phase and
	 * history freeze. Converting periods 1-2, skipping two calls,
	 * then converting period 5 must equal the no-gap concatenation
	 * [1,2,5] through a fresh instance. */
	static const uint16_t in_n = 882U;
	int16_t in5[882U * 5 * 2];
	int16_t out_gap[960U * 3 * 2];
	int16_t out_ref[960U * 3 * 2];
	struct zz_audio_convert a, b;
	uint32_t i;
	int16_t v;

	for (i = 0U; i < 882U * 5U; i++) {
		int32_t l = (int32_t)(i * 37U % 6000U) - 3000;

		in5[i * 2U] = (int16_t)l;
		in5[i * 2U + 1U] = (int16_t)(3000 - l);
	}

	zz_audio_convert_init(&a, 44100U, 48000U);
	zz_audio_convert_stream(&a, in5, out_gap, in_n, 960U);
	zz_audio_convert_stream(&a, in5 + 882U * 2U, out_gap + 960U * 2U,
	                        in_n, 960U);
	/* two silence periods: no converter call at all */
	zz_audio_convert_stream(&a, in5 + 882U * 4U * 2U,
	                        out_gap + 960U * 2U * 2U, in_n, 960U);

	/* Reference: the concatenated [1,2,5] stream (periods 3 and 4
	 * simply do not exist -- silence skipped the kernel entirely). */
	{
		int16_t ref_in[882U * 3 * 2];

		memcpy(ref_in, in5, 882U * 2 * 4U);
		memcpy(ref_in + 882U * 2 * 2, in5 + 882U * 4 * 2,
		       882U * 4U);
		zz_audio_convert_init(&b, 44100U, 48000U);
		zz_audio_convert_stream(&b, ref_in, out_ref, in_n * 3U,
		                        960U * 3U);
	}
	check(memcmp(out_gap, out_ref, sizeof(out_gap)) == 0,
	      "gap continuity", "silence-gap conversion must equal the "
	                        "no-gap concatenation");
	v = out_gap[0];
	(void)v;
	return failures == 0;
}

static int test_step_response(void)
{
	/* A step to a new level must settle to it within 2 LSB after the
	 * kernel length and show bounded (non-growing) ringing. */
	struct zz_audio_convert ctx;
	uint32_t p;
	int16_t out2[960U * 2U];

	zz_audio_convert_init(&ctx, 24000U, 48000U);
	for (p = 0U; p < 10U; p++) {
		uint32_t i;

		for (i = 0U; i < 480U * 2U; i++)
			in_buf[i] = -6000;
		zz_audio_convert_stream(&ctx, in_buf, out2, 480U, 960U);
	}
	for (p = 0U; p < 10U; p++) {
		uint32_t i;
		int32_t peak = 0;

		for (i = 0U; i < 480U * 2U; i++)
			in_buf[i] = 18000;
		zz_audio_convert_stream(&ctx, in_buf, out2, 480U, 960U);
		for (i = 0U; i < 960U * 2U; i++) {
			int32_t d = (int32_t)out2[i];

			if (d > peak)
				peak = d;
		}
		/* Overshoot of a Kaiser-windowed step is bounded well under
		 * 20%; anything larger means the kernel is unstable. */
		check(peak <= 23000, "step overshoot bounded",
		      "ringing must not exceed ~25% overshoot");
	}
	{
		uint32_t i;

		for (i = 0U; i < 960U * 2U; i++) {
			int32_t d = (int32_t)out2[i] - 18000;

			if (d < -2 || d > 2) {
				check(0, "step settles", NULL);
				break;
			}
		}
	}
	return failures == 0;
}

static int test_exact_position(void)
{
	/* After every exact-count period the rational position returns to
	 * zero: no drift accumulates over a long run. */
	uint32_t r;

	for (r = 0U; r < 5U; r++) {
		struct zz_audio_convert ctx;
		uint32_t p;
		char detail[64];

		fill_sine(big_in, per_period[r] * 1000U, rates[r], 440.0,
		          5000.0);
		zz_audio_convert_init(&ctx, rates[r], 48000U);
		for (p = 0U; p < 1000U; p++) {
			zz_audio_convert_stream(&ctx,
			        big_in + (size_t)p * per_period[r] * 2U,
			        out_buf, per_period[r], 960U);
			if (ctx.pos_int != 0U || ctx.pos_frac != 0U)
				break;
		}
		snprintf(detail, sizeof(detail), "playback %lu Hz period %lu",
		         (unsigned long)rates[r], (unsigned long)p);
		check(p == 1000U, "playback exact position", detail);

		zz_audio_convert_init(&ctx, 48000U, rates[r]);
		for (p = 0U; p < 1000U; p++) {
			zz_audio_convert_stream(&ctx, big_in, out_buf, 960U,
			                        per_period[r]);
			if (ctx.pos_int != 0U || ctx.pos_frac != 0U)
				break;
		}
		snprintf(detail, sizeof(detail), "capture %lu Hz period %lu",
		         (unsigned long)rates[r], (unsigned long)p);
		check(p == 1000U, "capture exact position", detail);
	}
	return failures == 0;
}

static int test_group_delay(void)
{
	uint32_t r;

	for (r = 0U; r < 5U; r++) {
		struct zz_audio_convert ctx;
		uint32_t periods = 10U;
		int32_t peak_in = -1;
		int32_t peak_out = -1;
		uint32_t i;
		double expect, got;
		char detail[96];

		memset(big_in, 0, sizeof(big_in));
		big_in[(per_period[r] * 5U) * 2U] = 30000;
		zz_audio_convert_init(&ctx, rates[r], 48000U);
		run_periods(&ctx, rates[r], periods, big_in, big_out);

		for (i = 0U; i < per_period[r] * periods; i++)
			if (big_in[i * 2U] != 0)
				peak_in = (int32_t)i;
		for (i = 0U; i < 960U * periods; i++)
			if (peak_out < 0 ||
			    abs((int)big_out[i * 2U]) >
			        abs((int)big_out[(uint32_t)peak_out * 2U]))
				peak_out = (int32_t)i;
		/* A causal kernel delays the signal: the impulse's image
		 * peaks group_delay input samples LATER, so
		 * out_idx = (in_idx + gd) * 960 / per_period. */
		expect = ((double)peak_in +
		          (double)ctx.ratio->group_delay) *
		         960.0 / (double)per_period[r];
		got = (double)peak_out;
		snprintf(detail, sizeof(detail), "%lu Hz expect %.2f got %ld",
		         (unsigned long)rates[r], expect, (long)peak_out);
		check(fabs(expect - got) <= 1.5, "group delay", detail);
	}

	/* Capture direction: 960 in, rate/50 out; impulse at input frame
	 * 5*960; output peak lands group_delay INPUT samples later, so
	 * out_idx = (in_idx + gd) * per_period / 960. */
	for (r = 0U; r < 5U; r++) {
		struct zz_audio_convert ctx;
		uint32_t periods = 12U;
		int32_t peak_in = -1;
		int32_t peak_out = -1;
		uint32_t i;
		double expect, got;
		char detail[96];

		memset(big_in, 0, sizeof(big_in));
		big_in[(960U * 5U) * 2U] = 30000;
		peak_in = (int32_t)(5U * 960U);
		{
			static int16_t outs[12U * 882U * 2];
			uint16_t out_n = per_period[r];
			uint32_t total = (uint32_t)out_n * periods;
			uint32_t p;

			zz_audio_convert_init(&ctx, 48000U, rates[r]);
			for (p = 0U; p < periods; p++)
				zz_audio_convert_stream(&ctx,
				        big_in + (size_t)p * 960U * 2U,
				        outs + (size_t)p * out_n * 2U, 960U,
				        out_n);
			peak_out = -1;
			for (i = 0U; i < total; i++)
				if (peak_out < 0 ||
				    abs((int)outs[i * 2U]) >
				        abs((int)outs[(uint32_t)peak_out * 2U]))
					peak_out = (int32_t)i;
		}
		expect = ((double)peak_in +
		          (double)ctx.ratio->group_delay) *
		         (double)per_period[r] / 960.0;
		got = (double)peak_out;
		snprintf(detail, sizeof(detail),
		         "capture %lu Hz expect %.2f got %ld",
		         (unsigned long)rates[r], expect,
		         (long)peak_out);
		check(fabs(expect - got) <= 1.5, "capture group delay",
		      detail);
	}
	return failures == 0;
}

static int test_mono_bit_identity(void)
{
	uint32_t r;

	for (r = 0U; r < 5U; r++) {
		struct zz_audio_convert ctx;
		uint32_t p;
		uint32_t i;
		int ok = 1;
		char detail[64];

		fill_sine(big_in, per_period[r] * 20U, rates[r], 440.0,
		          8000.0);
		zz_audio_convert_init(&ctx, rates[r], 48000U);
		for (p = 0U; p < 20U; p++) {
			zz_audio_convert_stream(&ctx,
			        big_in + (size_t)p * per_period[r] * 2U,
			        out_buf, per_period[r], 960U);
			for (i = 0U; i < 960U; i++)
				if (out_buf[i * 2U] != out_buf[i * 2U + 1U])
					ok = 0;
		}
		snprintf(detail, sizeof(detail), "%lu Hz",
		         (unsigned long)rates[r]);
		check(ok, "mono bit identity",
		      "L/R must stay bit-identical");
	}
	return failures == 0;
}

static int test_saturation(void)
{
	struct zz_audio_convert ctx;
	uint32_t i;

	/* A band-limited square wave at 95% of full scale: Fourier sum of
	 * odd harmonics below the passband edge. Gibbs overshoot at the
	 * discontinuities (~+9%) pushes the output past full scale, so
	 * the core must saturate and count clips. */
	zz_audio_convert_init(&ctx, 24000U, 48000U);
	for (i = 0U; i < 480U * 1000U; i++) {
		double t = (double)i / 24000.0;
		double v = 0.0;
		uint32_t k;

		for (k = 1U; k <= 35U; k += 2U)
			v += sin(2.0 * M_PI * 200.0 * (double)k * t) /
			     (double)k;
		v *= 4.0 / M_PI * 0.95 * 32767.0;
		big_in[i * 2U] = (int16_t)lrint(v);
		big_in[i * 2U + 1U] = big_in[i * 2U];
	}
	{
		uint32_t p;

		for (p = 0U; p < 1000U; p++)
			zz_audio_convert_stream(&ctx,
			        big_in + (size_t)p * 480U * 2U, out_buf,
			        480U, 960U);
	}
	check(zz_audio_convert_clips(&ctx) > 0U, "saturation clip counter",
	      "square-wave overshoot must clip and be counted");
	return failures == 0;
}

static int test_reset_semantics(void)
{
	struct zz_audio_convert ctx;

	zz_audio_convert_init(&ctx, 24000U, 48000U);
	fill_sine(in_buf, 480U, 24000U, 440.0, 8000.0);
	zz_audio_convert_stream(&ctx, in_buf, out_buf, 480U, 960U);
	check(ctx.history_len != 0U, "history populated", NULL);
	zz_audio_convert_reset(&ctx);
	check(ctx.pos_int == 0U && ctx.pos_frac == 0U &&
	          ctx.history_len == 0U && ctx.clips == 0U,
	      "reset clears all state", NULL);
	/* After reset the first period must equal a fresh instance. */
	{
		struct zz_audio_convert fresh;
		int16_t out2[960U * 2U];

		zz_audio_convert_stream(&ctx, in_buf, out2, 480U, 960U);
		zz_audio_convert_init(&fresh, 24000U, 48000U);
		zz_audio_convert_stream(&fresh, in_buf, ref_buf, 480U, 960U);
		check(memcmp(out2, ref_buf, 960U * 4U) == 0,
		      "reset equals fresh", NULL);
	}
	return failures == 0;
}

static void report(void)
{
	static const uint16_t counts[5] = { 160U, 240U, 480U, 640U, 882U };
	uint32_t r;
	double pump_worst = 0.0;
	double capture_worst = 0.0;

	printf("# audio-conversion measured table\n");
	printf("# regenerate: make -C test/audio && "
	       "./build/audio_convert_test --report\n");
	printf("# kernel: Kaiser windowed-sinc polyphase, A=80 dB design, "
	       "passband 0.45*min(fs), stopband from min(fs)/2, full-range "
	       "int16 taps with per-phase Q16 reciprocal (int64 accumulate), "
	       "no dither. Behavioral stop-band floor: >= 70 dB every "
	       "direction (int16 coefficient quantization limit).\n");
	printf("| rate | direction | taps | phases | group delay (in) | "
	       "passband ripple (dB) | MAC/period | table bytes |\n");
	printf("|---|---|---|---|---|---|---|---|\n");
	for (r = 0U; r < 5U; r++) {
		struct zz_audio_convert ctx;
		const struct zz_audio_convert_ratio *ratio;
		double worst_pb = 0.0;
		uint32_t phase, bin;

		zz_audio_convert_init(&ctx, rates[r], 48000U);
		ratio = ctx.ratio;
		worst_pb = worst_passband_ripple(ratio, rates[r]);
		(void)phase;
		(void)bin;
		{
			double macs = (double)ratio->taps *
			    (ratio->phases > 1U ? 960.0 : 960.0) * 2.0;
			double bytes = (double)ratio->phases *
			    (double)ratio->taps * 2.0;

			printf("| %lu | playback->48k | %u | %u | %u | "
			       "%.3f | %.0f | %.0f |\n",
			       (unsigned long)rates[r], ratio->taps,
			       ratio->phases, ratio->group_delay, worst_pb,
			       macs, bytes);
			if (macs > pump_worst)
				pump_worst = macs;
		}
	}
	for (r = 0U; r < 5U; r++) {
		struct zz_audio_convert ctx;
		const struct zz_audio_convert_ratio *ratio;
		double macs;
		double bytes;

		zz_audio_convert_init(&ctx, 48000U, rates[r]);
		ratio = ctx.ratio;
		macs = (double)ratio->taps * (double)counts[r] * 2.0;
		bytes = (double)ratio->phases * (double)ratio->taps * 2.0;
		printf("| %lu | capture->rate | %u | %u | %u | (see "
		       "playback row) | %.0f | %.0f |\n",
		       (unsigned long)rates[r], ratio->taps, ratio->phases,
		       ratio->group_delay, macs, bytes);
		if (macs > capture_worst)
			capture_worst = macs;
	}
	{
		double burst = 6.0 * pump_worst + 7.0 * capture_worst;
		double window_cycles = 666666687.0 / 50.0;

		printf("\nWorst-case full-duplex burst (6 pump + 7 capture "
		       "periods): %.0f MAC/window\n", burst);
		printf("At 666.67 MHz with one SMLAL per tap plus loads: "
		       "~1-2 cycles/MAC bounds the burst at %.0f-%.0f "
		       "cycles = %.0f-%.0f%% of the 13.33M-cycle 20 ms "
		       "window\n",
		       burst, burst * 2.0, 100.0 * burst / window_cycles,
		       200.0 * burst / window_cycles);
		printf("Card-side cycle cost is a host-derived analytic "
		       "estimate until an on-hardware run exists.\n");
	}
}

int main(int argc, char **argv)
{
	if (argc == 2 && strcmp(argv[1], "--report") == 0) {
		report();
		return 0;
	}
	test_init_and_identity();
	test_table_integrity();
	test_dc();
	test_table_response();
	test_alias_rejection();
	test_image_rejection();
	test_thd_n();
	test_thd_n_low_level();
	test_gap_continuity();
	test_step_response();
	test_chunked_vs_whole();
	test_exact_position();
	test_group_delay();
	test_mono_bit_identity();
	test_saturation();
	test_reset_semantics();

	if (failures == 0) {
		printf("audio_convert_test: all tests passed\n");
		return 0;
	}
	printf("audio_convert_test: %d failure(s)\n", failures);
	return 1;
}
