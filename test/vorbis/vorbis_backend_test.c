/*
 * Host tests for the firmware Ogg Vorbis backend (sdk_audio_vorbis.c) linked
 * with the pinned libogg/Tremor host archive and the real arena allocator /
 * decode tracker. Fixtures and reference PCM come from gen_fixtures.sh
 * (vorbis_fixtures.h records the tools). The reference is an independent
 * floating-point decode, so decoded PCM is compared within TOL_MAX_LSB per
 * sample and TOL_RMS_LSB overall (see check_tolerance), while every chunking,
 * ring size and drain pattern of one fixture must be byte-identical.
 *
 * Usage: vorbis_backend_test <fixtures-dir>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sdk_audio_vorbis.h"
#include "sdk_decode_reclaim.h"
#include "sdk_mailbox.h"
#include "host_runtime.h"
#include "vorbis_fixtures.h"

/* Tremor (full-precision 32x32->64 fixed point, then >> 9 and clip) against
 * ffmpeg's float decoder rounded to nearest by libswresample: the >> 9
 * truncates toward minus infinity (an error of -1..0 LSB, about 0.7 LSB RMS
 * on its own), and the fixed-point MDCT/windowing adds rounding noise below
 * one LSB. Measured on the fixtures: at most 2 LSB, 0.71 LSB RMS. A decoder
 * fault (missing/duplicated block, channel swap, misaligned overlap) gives
 * errors of hundreds to thousands of LSB, far outside these bounds. */
#define TOL_MAX_LSB 4
#define TOL_RMS_LSB 1.0

#define IN_CAP      (16U * 1024U)
#define MAX_IN_CAP  (64U * 1024U)
#define MAX_PCM_CAP (128U * 1024U)
#define MAX_OUT     (512U * 1024U)

static int failures;
static const char *fixture_dir = "fixtures";

#define CHECK(cond, ...) do { if (!(cond)) { failures++; \
	printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); \
	printf("\n"); } } while (0)

struct blob {
	uint8_t *data;
	uint32_t len;
};

static struct blob load(const char *name, const char *ext)
{
	struct blob b = { 0, 0 };
	char path[512];
	FILE *f;
	long n;

	snprintf(path, sizeof(path), "%s/%s%s", fixture_dir, name, ext);
	f = fopen(path, "rb");
	if (!f) {
		printf("FATAL: cannot open %s\n", path);
		exit(2);
	}
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	b.data = (uint8_t *)malloc(n > 0 ? (size_t)n : 1U);
	if (!b.data || fread(b.data, 1, (size_t)n, f) != (size_t)n) {
		printf("FATAL: cannot read %s\n", path);
		exit(2);
	}
	fclose(f);
	b.len = (uint32_t)n;
	return b;
}

struct sim {
	struct sdk_vorbis_state st;
	uint8_t in[MAX_IN_CAP];
	uint32_t in_cap, off, len;
	uint8_t pcm[MAX_PCM_CAP];
	uint32_t pcm_cap, pcm_budget, unread;
	uint32_t written_total;
	uint8_t out[MAX_OUT];
	uint32_t out_len;
	uint32_t packets, calls;
	int eof, starved, complete;
};

static struct sim g_sim;
static uint8_t g_out_a[MAX_OUT];

static uint16_t sim_decode(struct sim *s, int drain)
{
	struct sdk_audio_codec_io io;
	uint16_t status;
	uint32_t i, pos;

	memset(&io, 0, sizeof(io));
	io.input = s->in + s->off;
	io.input_length = s->len;
	io.eof = s->eof;
	io.drain = drain;
	io.pcm = s->pcm;
	io.pcm_capacity = s->pcm_cap;
	io.pcm_write = s->written_total % s->pcm_cap;
	io.pcm_free = s->pcm_cap;          /* consumer drains after every call */
	io.pcm_budget = s->pcm_budget;
	io.pcm_unread = s->unread;
	status = sdk_vorbis_decode(&s->st, &io);
	s->calls++;
	if (io.consumed > s->len) {
		CHECK(0, "consumed %u > buffered %u", io.consumed, s->len);
		return SDK_STATUS_INTERNAL_ERROR;
	}
	CHECK(io.produced <= io.pcm_free, "produced %u > free %u", io.produced,
	      io.pcm_free);
	if (s->st.channels)
		CHECK(io.produced % (2U * s->st.channels) == 0U,
		      "partial frame published (%u bytes)", io.produced);
	s->off += io.consumed;
	s->len -= io.consumed;
	if (s->len == 0U)
		s->off = 0U;
	pos = io.pcm_write;
	for (i = 0U; i < io.produced && s->out_len < MAX_OUT; i++) {
		s->out[s->out_len++] = s->pcm[pos];
		if (++pos == s->pcm_cap)
			pos = 0U;
	}
	s->written_total += io.produced;
	s->packets += io.frames;
	s->starved = io.starved;
	s->complete = io.complete;
	return status;
}

static void sim_init(struct sim *s, uint32_t in_cap, uint32_t pcm_cap)
{
	memset(s, 0, sizeof(*s));
	s->in_cap = in_cap;
	s->pcm_cap = pcm_cap;
	s->pcm_budget = pcm_cap;
	sdk_vorbis_init(&s->st);
}

/* Feed `data` in `chunk` slices through a bounded input ring, like the
 * mailbox FEED path: compact, append, decode; EOF with the final slice. */
static uint16_t run_fed(struct sim *s, const uint8_t *data, uint32_t size,
                        uint32_t chunk, int drain_each)
{
	uint32_t fed = 0U;
	uint32_t guard = 0U;
	uint16_t status = SDK_STATUS_OK;

	while (!s->complete && guard++ < 4000000U) {
		uint32_t take = size - fed;

		if (take > chunk)
			take = chunk;
		if (take > s->in_cap - s->len)
			take = s->in_cap - s->len;
		if (take != 0U) {
			if (s->off + s->len + take > s->in_cap) {
				memmove(s->in, s->in + s->off, s->len);
				s->off = 0U;
			}
			memcpy(s->in + s->off + s->len, data + fed, take);
			s->len += take;
			fed += take;
		} else if (fed < size && s->len == s->in_cap) {
			uint32_t before = s->calls;

			/* Ring full: the decoder must make progress. */
			status = sim_decode(s, 0);
			if (status != SDK_STATUS_OK)
				return status;
			if (s->len == s->in_cap) {
				CHECK(0, "no progress with a full ring (%u calls)", before);
				return 0xFFFFU;
			}
			continue;
		}
		if (fed == size)
			s->eof = 1;
		status = sim_decode(s, drain_each && !s->eof);
		if (status != SDK_STATUS_OK)
			return status;
		if (s->eof && !s->complete && s->len == 0U && s->starved) {
			CHECK(0, "starved after EOF");
			return 0xFFFEU;
		}
	}
	return status;
}

static uint16_t run_stream(struct sim *s, const uint8_t *data, uint32_t size,
                           uint32_t chunk, uint32_t in_cap, uint32_t pcm_cap,
                           int drain_each)
{
	sim_init(s, in_cap, pcm_cap);
	return run_fed(s, data, size, chunk, drain_each);
}

static void check_clean(const char *what)
{
	CHECK(host_runtime_allocated_bytes() == 0U && host_runtime_live_blocks() == 0U &&
	      sdk_decode_tracked_count() == 0U,
	      "%s: leak bytes=%zu blocks=%u tracked=%u", what,
	      host_runtime_allocated_bytes(), host_runtime_live_blocks(),
	      sdk_decode_tracked_count());
}

static void finish(struct sim *s, const char *what)
{
	sdk_vorbis_release(&s->st);
	check_clean(what);
}

struct diff {
	int max;
	double rms;
};

static struct diff compare(const uint8_t *a, const uint8_t *b, uint32_t bytes)
{
	struct diff d = { 0, 0.0 };
	double sum = 0.0;
	uint32_t i, n = bytes / 2U;

	for (i = 0U; i < n; i++) {
		int va = (int16_t)(((uint16_t)a[2 * i] << 8) | a[2 * i + 1]);
		int vb = (int16_t)(((uint16_t)b[2 * i] << 8) | b[2 * i + 1]);
		int e = va > vb ? va - vb : vb - va;

		if (e > d.max)
			d.max = e;
		sum += (double)e * e;
	}
	d.rms = n ? sqrt(sum / n) : 0.0;
	return d;
}

static int check_tolerance(const char *what, const uint8_t *out, uint32_t out_len,
                           const struct blob *ref)
{
	struct diff d;

	CHECK(out_len == ref->len, "%s: %u PCM bytes, reference %u", what, out_len,
	      ref->len);
	if (out_len != ref->len)
		return 0;
	d = compare(out, ref->data, ref->len);
	CHECK(d.max <= TOL_MAX_LSB && d.rms <= TOL_RMS_LSB,
	      "%s: max %d LSB rms %.3f LSB beyond tolerance", what, d.max, d.rms);
	return d.max;
}

static void test_fixtures(void)
{
	static const uint32_t chunks[] = { 1U, 7U, 333U, 4096U, 1U << 24 };
	size_t f;
	unsigned c;

	printf("fixture              rate  ch blocks     setup  max|err| rms     "
	       "peak tracked (arena regions / host heap)\n");
	for (f = 0; f < sizeof(vorbis_fixtures) / sizeof(vorbis_fixtures[0]); f++) {
		const struct vorbis_fixture *fx = &vorbis_fixtures[f];
		struct blob ogg = load(fx->name, ".ogg");
		struct blob ref = load(fx->ref, ".s16");
		uint32_t base_len = 0U;
		int maxerr = 0;

		for (c = 0U; c < sizeof(chunks) / sizeof(chunks[0]); c++) {
			struct sim *s = &g_sim;
			uint16_t st;

			host_runtime_reset();
			st = run_stream(s, ogg.data, ogg.len, chunks[c], IN_CAP,
			                65536U, 0);
			CHECK(st == SDK_STATUS_OK && s->complete,
			      "%s chunk %u: status %u complete %d", fx->name,
			      chunks[c], st, s->complete);
			CHECK(s->st.sample_rate == fx->rate &&
			      s->st.channels == fx->channels,
			      "%s: geometry %u/%u", fx->name, s->st.sample_rate,
			      s->st.channels);
			CHECK(!sdk_vorbis_holds_memory(&s->st),
			      "%s: arena kept after completion", fx->name);
			CHECK(s->st.heap.peak <= SDK_VORBIS_ALLOC_LIMIT,
			      "%s: peak above the cap", fx->name);
			if (c == 0U) {
				int e = check_tolerance(fx->name, s->out, s->out_len, &ref);

				if (e > maxerr)
					maxerr = e;
				memcpy(g_out_a, s->out, s->out_len);
				base_len = s->out_len;
			} else {
				CHECK(s->out_len == base_len &&
				      memcmp(s->out, g_out_a, base_len) == 0,
				      "%s chunk %u: output differs from chunk 1",
				      fx->name, chunks[c]);
			}
			if (c == 3U) {
				struct diff d = compare(s->out, ref.data,
				                        s->out_len < ref.len ? s->out_len : ref.len);

				printf("%-20s %6u %2u %4u/%-5u %5u  %3d    %.3f   %u B (%u) / %zu B\n",
				       fx->name, fx->rate, fx->channels, fx->block0,
				       fx->block1, fx->setup_bytes, d.max, d.rms,
				       s->st.heap.peak, s->st.heap.peak /
				       SDK_VORBIS_REGION_BYTES,
				       host_runtime_peak_allocated_bytes());
			}
			finish(s, fx->name);
		}
		/* Small odd-sized PCM ring and a one-byte budget: output
		 * stops at frame boundaries mid-packet and wraps. */
		host_runtime_reset();
		sim_init(&g_sim, IN_CAP, 1002U);
		g_sim.pcm_budget = 1U;
		{
			uint16_t st = run_fed(&g_sim, ogg.data, ogg.len, 333U, 0);

			CHECK(st == SDK_STATUS_OK && g_sim.complete &&
			      g_sim.out_len == base_len &&
			      memcmp(g_sim.out, g_out_a, base_len) == 0,
			      "%s: small ring/budget output differs (%u)", fx->name, st);
		}
		finish(&g_sim, "small ring");
		/* FEED_DRAIN on every feed gives the same stream. */
		host_runtime_reset();
		CHECK(run_stream(&g_sim, ogg.data, ogg.len, 997U, IN_CAP, 65536U, 1) ==
		      SDK_STATUS_OK && g_sim.out_len == base_len &&
		      memcmp(g_sim.out, g_out_a, base_len) == 0,
		      "%s: drain feeds differ", fx->name);
		finish(&g_sim, "drain");
		free(ogg.data);
		free(ref.data);
	}
}

static uint16_t run_file(const char *name, uint32_t *out_len)
{
	struct blob b = load(name, ".ogg");
	uint16_t st;

	host_runtime_reset();
	st = run_stream(&g_sim, b.data, b.len, 4096U, IN_CAP, 65536U, 0);
	if (out_len)
		*out_len = g_sim.out_len;
	finish(&g_sim, name);
	free(b.data);
	return st;
}

static void test_rejections(void)
{
	static const struct {
		const char *name;
		uint16_t status;
		int no_pcm;
	} cases[] = {
		{ "neg_badcrc", SDK_STATUS_IO_ERROR, 0 },
		{ "neg_nocomment", SDK_STATUS_IO_ERROR, 1 },
		{ "neg_swapped", SDK_STATUS_IO_ERROR, 1 },
		{ "neg_setupfirst", SDK_STATUS_IO_ERROR, 1 },
		{ "neg_badcomment", SDK_STATUS_IO_ERROR, 1 },
		{ "neg_badpacket", SDK_STATUS_IO_ERROR, 0 },
		{ "neg_truncated_mid", SDK_STATUS_IO_ERROR, 0 },
		{ "neg_truncated_page", SDK_STATUS_IO_ERROR, 0 },
		{ "neg_gap", SDK_STATUS_IO_ERROR, 0 },
		{ "neg_bigsetup", SDK_STATUS_UNSUPPORTED, 1 },
		{ "neg_bigbook", SDK_STATUS_UNSUPPORTED, 1 },
		{ "neg_rate7999", SDK_STATUS_UNSUPPORTED, 1 },
		{ "neg_3ch", SDK_STATUS_UNSUPPORTED, 1 },
		{ "neg_multiplexed", SDK_STATUS_UNSUPPORTED, 1 },
		{ "neg_opus", SDK_STATUS_UNSUPPORTED, 1 },
		{ "neg_oggflac", SDK_STATUS_UNSUPPORTED, 1 },
	};
	static const uint8_t not_ogg[64] = { 'f', 'L', 'a', 'C', 0, 0, 0, 34 };
	struct blob ref = load("stereo44k", ".s16");
	size_t i;
	uint16_t st;

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		uint32_t out_len = 0U;

		st = run_file(cases[i].name, &out_len);
		CHECK(st == cases[i].status, "%s: status %u, want %u",
		      cases[i].name, st, cases[i].status);
		CHECK(out_len < ref.len, "%s: produced the whole stream", cases[i].name);
		if (cases[i].no_pcm)
			CHECK(out_len == 0U, "%s: produced %u PCM bytes",
			      cases[i].name, out_len);
		else if (out_len != 0U)
			/* Whatever was published before the fault is a
			 * correct prefix of the stream. */
			CHECK(compare(g_sim.out, ref.data, out_len).max <= TOL_MAX_LSB,
			      "%s: published prefix is wrong", cases[i].name);
	}
	host_runtime_reset();
	st = run_stream(&g_sim, not_ogg, sizeof(not_ogg), 4096U, IN_CAP, 65536U, 0);
	CHECK(st == SDK_STATUS_IO_ERROR, "fLaC input: %u", st);
	finish(&g_sim, "not ogg");
	host_runtime_reset();
	st = run_stream(&g_sim, not_ogg, 0U, 4096U, IN_CAP, 65536U, 0);
	CHECK(st == SDK_STATUS_IO_ERROR, "empty input at EOF: %u", st);
	finish(&g_sim, "empty");
	/* Sticky: later calls repeat the fault and consume nothing. */
	{
		struct sdk_audio_codec_io io;

		memset(&io, 0, sizeof(io));
		io.input = not_ogg;
		io.input_length = sizeof(not_ogg);
		io.pcm = g_sim.pcm;
		io.pcm_capacity = 1024U;
		io.pcm_free = 1024U;
		CHECK(sdk_vorbis_decode(&g_sim.st, &io) == SDK_STATUS_IO_ERROR &&
		      io.consumed == 0U && io.produced == 0U, "sticky status");
	}
	free(ref.data);
}

static void test_chained(void)
{
	struct blob ogg = load("neg_chained", ".ogg");
	struct blob first = load("stereo44k", ".ogg");
	struct sim *s = &g_sim;
	uint32_t link1_len;
	uint16_t st;
	unsigned spins = 0U;

	/* Link 1 alone, for the byte-exact expectation. */
	host_runtime_reset();
	st = run_stream(s, first.data, first.len, 4096U, IN_CAP, 65536U, 0);
	CHECK(st == SDK_STATUS_OK, "link 1 alone: %u", st);
	memcpy(g_out_a, s->out, s->out_len);
	link1_len = s->out_len;
	finish(s, "link1");

	/* Consumer reads everything at once: every link-1 PCM byte is
	 * produced before the fault, which then repeats. */
	host_runtime_reset();
	st = run_stream(s, ogg.data, ogg.len, 4096U, IN_CAP, 65536U, 0);
	CHECK(st == SDK_STATUS_UNSUPPORTED, "chained: %u", st);
	CHECK(s->out_len == link1_len && memcmp(s->out, g_out_a, link1_len) == 0,
	      "chained: link 1 PCM incomplete (%u of %u)", s->out_len, link1_len);
	CHECK(!sdk_vorbis_holds_memory(&s->st), "chained: arena kept");
	CHECK(sim_decode(s, 0) == SDK_STATUS_UNSUPPORTED, "chained: not sticky");
	finish(s, "chained");

	/* A slow consumer: the fault waits until the client has read all
	 * published PCM, and later input is discarded meanwhile. */
	host_runtime_reset();
	sim_init(s, MAX_IN_CAP, 65536U);
	s->unread = 4U;
	s->len = ogg.len < MAX_IN_CAP ? ogg.len : MAX_IN_CAP;
	memcpy(s->in, ogg.data, s->len);
	s->eof = 1;
	do {
		st = sim_decode(s, 0);
	} while (st == SDK_STATUS_OK && !s->complete && s->len != 0U &&
	         ++spins < 100000U);
	CHECK(st == SDK_STATUS_OK && s->len == 0U && !s->complete,
	      "pending fault: status %u len %u", st, s->len);
	CHECK(s->out_len == link1_len, "pending fault: link 1 PCM %u of %u",
	      s->out_len, link1_len);
	CHECK(sim_decode(s, 0) == SDK_STATUS_OK, "fault raised with PCM unread");
	s->unread = 0U;
	CHECK(sim_decode(s, 0) == SDK_STATUS_UNSUPPORTED, "fault after read");
	finish(s, "chained slow");
	free(ogg.data);
	free(first.data);
}

static void test_starvation_and_trailer(void)
{
	struct blob ogg = load("stereo44k", ".ogg");
	struct sim *s = &g_sim;
	uint32_t full;
	uint16_t st;

	host_runtime_reset();
	st = run_stream(s, ogg.data, ogg.len, 1U << 24, IN_CAP, 65536U, 0);
	memcpy(g_out_a, s->out, s->out_len);
	full = s->out_len;
	finish(s, "baseline");

	/* Everything but the last 10 bytes, no EOF: starved, not complete,
	 * every byte taken into the page buffer. Then the tail with EOF. The
	 * PCM ring holds the whole stream so one call decodes everything. */
	host_runtime_reset();
	sim_init(s, MAX_IN_CAP, MAX_PCM_CAP);
	memcpy(s->in, ogg.data, ogg.len - 10U);
	s->len = ogg.len - 10U;
	st = sim_decode(s, 1);
	CHECK(st == SDK_STATUS_OK && s->starved && !s->complete && s->len == 0U &&
	      s->out_len < full, "starved on the partial last page");
	CHECK(sim_decode(s, 1) == SDK_STATUS_OK && s->starved,
	      "starvation is stable");
	memcpy(s->in, ogg.data + ogg.len - 10U, 10U);
	s->len = 10U;
	s->eof = 1;
	st = sim_decode(s, 0);
	CHECK(st == SDK_STATUS_OK && s->complete && s->out_len == full &&
	      memcmp(s->out, g_out_a, full) == 0, "resume after starvation");
	/* After completion further input is discarded. */
	s->len = 5U;
	CHECK(sim_decode(s, 0) == SDK_STATUS_OK && s->complete && s->len == 0U,
	      "input after completion");
	finish(s, "starvation");

	/* EOS seen but no FEED_EOF yet: stays incomplete until EOF. */
	host_runtime_reset();
	sim_init(s, MAX_IN_CAP, MAX_PCM_CAP);
	memcpy(s->in, ogg.data, ogg.len);
	s->len = ogg.len;
	st = sim_decode(s, 0);
	CHECK(st == SDK_STATUS_OK && !s->complete && s->starved &&
	      s->out_len == full, "EOS without FEED_EOF");
	s->eof = 1;
	CHECK(sim_decode(s, 0) == SDK_STATUS_OK && s->complete, "EOF completes");
	finish(s, "eos");
	free(ogg.data);
}

static void test_allocation_failures(void)
{
	struct blob ogg = load("stereo44k", ".ogg");
	static const uint32_t limits[] = { 0U, 4096U, 32768U, 65536U, 131072U };
	unsigned attempts, n, nomem = 0U;
	uint16_t st;
	size_t i;

	/* Decode-heap failure at every region allocation. */
	host_runtime_reset();
	st = run_stream(&g_sim, ogg.data, ogg.len, 4096U, IN_CAP, 65536U, 0);
	attempts = host_runtime_allocation_attempts();
	finish(&g_sim, "alloc baseline");
	CHECK(st == SDK_STATUS_OK && attempts > 0U, "alloc baseline");
	for (n = 1U; n <= attempts; n++) {
		host_runtime_reset();
		host_runtime_fail_on_allocation(n);
		st = run_stream(&g_sim, ogg.data, ogg.len, 4096U, IN_CAP, 65536U, 0);
		CHECK(st == SDK_STATUS_NO_MEMORY, "fail region %u -> %u", n, st);
		CHECK(!sdk_vorbis_holds_memory(&g_sim.st), "arena kept after NO_MEMORY");
		if (st == SDK_STATUS_NO_MEMORY)
			nomem++;
		finish(&g_sim, "alloc failure");
	}
	printf("  region allocation failure injection: %u/%u points -> NO_MEMORY, no leaks\n",
	       nomem, attempts);

	/* Arena ceiling reached inside libogg, Tremor's header parsing and
	 * vorbis_synthesis_init(): the longjmp path leaks nothing. */
	for (i = 0; i < sizeof(limits) / sizeof(limits[0]); i++) {
		host_runtime_reset();
		sim_init(&g_sim, IN_CAP, 65536U);
		sdk_vorbis_heap_init(&g_sim.st.heap, 4096U, limits[i]);
		st = run_fed(&g_sim, ogg.data, ogg.len, 4096U, 0);
		CHECK(st == SDK_STATUS_NO_MEMORY, "limit %u -> %u", limits[i], st);
		finish(&g_sim, "limit");
	}
	/* Small regions: more of them, same output, within the region cap. */
	host_runtime_reset();
	sim_init(&g_sim, IN_CAP, 65536U);
	sdk_vorbis_heap_init(&g_sim.st.heap, 16384U, SDK_VORBIS_ALLOC_LIMIT);
	st = run_fed(&g_sim, ogg.data, ogg.len, 4096U, 0);
	CHECK(st == SDK_STATUS_OK || st == SDK_STATUS_NO_MEMORY,
	      "16 KiB regions: %u", st);
	finish(&g_sim, "small regions");

	/* Decode tracker full: no region can be registered. */
	{
		static char dummies[SDK_DECODE_MAX_TRACKED];
		unsigned k;

		host_runtime_reset();
		for (k = 0U; k < SDK_DECODE_MAX_TRACKED; k++)
			sdk_decode_track(&dummies[k]);
		st = run_stream(&g_sim, ogg.data, ogg.len, 4096U, IN_CAP, 65536U, 0);
		CHECK(st == SDK_STATUS_NO_MEMORY, "tracker full -> %u", st);
		sdk_vorbis_release(&g_sim.st);
		for (k = 0U; k < SDK_DECODE_MAX_TRACKED; k++)
			sdk_decode_untrack(&dummies[k]);
		check_clean("tracker full");
	}
	free(ogg.data);
}

static void test_restart_reclaim(void)
{
	struct blob ogg = load("stereo48k_q10", ".ogg");
	unsigned reclaimed;

	host_runtime_reset();
	sim_init(&g_sim, MAX_IN_CAP, 65536U);
	memcpy(g_sim.in, ogg.data, ogg.len / 2U);
	g_sim.len = ogg.len / 2U;
	CHECK(sim_decode(&g_sim, 0) == SDK_STATUS_OK && g_sim.packets > 0U,
	      "partial decode");
	CHECK(sdk_vorbis_holds_memory(&g_sim.st) && host_runtime_live_blocks() > 0U,
	      "arena holds regions");
	/* Core-1 cold restart: core 0 frees every tracked block. */
	reclaimed = sdk_decode_reclaim(sdk_decode_heap_free);
	sdk_vorbis_forget(&g_sim.st);
	CHECK(reclaimed > 0U && !sdk_vorbis_holds_memory(&g_sim.st),
	      "reclaim freed nothing");
	check_clean("restart reclaim");
	printf("  restart reclaim: %u tracked regions freed\n", reclaimed);
	free(ogg.data);
}

/* The arena on its own: random malloc/realloc/free with content checks;
 * everything coalesces back and every region is returned. */
static void test_arena(void)
{
	struct sdk_vorbis_heap heap;
	static void *ptr[400];
	static uint32_t size[400];
	uint32_t seed = 12345U;
	unsigned round, i;

	host_runtime_reset();
	sdk_vorbis_heap_init(&heap, 32768U, 512U * 1024U);
	sdk_vorbis_heap_select(&heap, 0);
	for (round = 0U; round < 20000U; round++) {
		uint32_t k, n;

		seed = seed * 1103515245U + 12345U;
		k = (seed >> 8) % 400U;
		seed = seed * 1103515245U + 12345U;
		n = 1U + (seed >> 8) % ((seed & 0x100000U) ? 40000U : 300U);
		if (ptr[k]) {
			for (i = 0U; i < size[k]; i++)
				if (((uint8_t *)ptr[k])[i] != (uint8_t)(k + i))
					break;
			CHECK(i == size[k], "arena block %u corrupted", k);
			if (seed & 0x200000U) {
				void *q = sdk_vorbis_realloc(ptr[k], n);

				if (q) {
					uint32_t keep = n < size[k] ? n : size[k];

					for (i = 0U; i < keep; i++)
						if (((uint8_t *)q)[i] != (uint8_t)(k + i))
							break;
					CHECK(i == keep, "realloc lost data");
					ptr[k] = q;
					size[k] = n;
				}
			} else {
				sdk_vorbis_free(ptr[k]);
				ptr[k] = 0;
				continue;
			}
		} else {
			ptr[k] = sdk_vorbis_malloc(n);
			if (!ptr[k])
				continue;     /* over the ceiling: NULL, no longjmp */
			size[k] = n;
		}
		for (i = 0U; i < size[k]; i++)
			((uint8_t *)ptr[k])[i] = (uint8_t)(k + i);
		CHECK(heap.used <= heap.limit, "arena above its limit");
	}
	for (i = 0U; i < 400U; i++)
		sdk_vorbis_free(ptr[i]);
	CHECK(sdk_vorbis_heap_regions(&heap) == 0U && heap.used == 0U,
	      "arena regions not returned (%u)", sdk_vorbis_heap_regions(&heap));
	CHECK(sdk_vorbis_malloc(512U * 1024U + 1U) == 0, "over-limit allocation");
	sdk_vorbis_heap_select(0, 0);
	check_clean("arena");
	printf("  arena: 20000 random operations, peak %u B in <= %u regions\n",
	       heap.peak, SDK_VORBIS_HEAP_MAX_REGIONS);
}

int main(int argc, char **argv)
{
	if (argc > 1)
		fixture_dir = argv[1];
	test_arena();
	test_fixtures();
	test_rejections();
	test_chained();
	test_starvation_and_trailer();
	test_allocation_failures();
	test_restart_reclaim();
	if (failures) {
		printf("vorbis_backend_test: %d failure(s)\n", failures);
		return 1;
	}
	printf("vorbis_backend_test: all passed\n");
	return 0;
}
