/*
 * Host tests for the firmware FLAC backend (sdk_audio_flac.c) linked with the
 * pinned decoder-only libFLAC host archive and the real allocation shim /
 * decode tracker. Reference PCM comes from the reference `flac` tool (see
 * flac_fixtures.h); every decoded byte is compared exactly.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sdk_audio_flac.h"
#include "sdk_decode_reclaim.h"
#include "sdk_mailbox.h"
#include "host_runtime.h"
#include "flac_fixtures.h"

#define MAX_IN_CAP  (64U * 1024U)
#define MAX_PCM_CAP (256U * 1024U)
#define MAX_OUT     (1024U * 1024U)

static int failures;

#define CHECK(cond, ...) do { if (!(cond)) { failures++; \
	printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); \
	printf("\n"); } } while (0)

struct sim {
	struct sdk_flac_state st;
	uint8_t in[MAX_IN_CAP];
	uint32_t in_cap, off, len;
	uint8_t pcm[MAX_PCM_CAP];
	uint32_t pcm_cap, pcm_budget;
	uint32_t written_total;
	uint8_t out[MAX_OUT];
	uint32_t out_len;
	uint32_t frames;
	int eof, starved, complete;
};

static struct sim g_sim;

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
	status = sdk_flac_decode(&s->st, &io);
	if (io.consumed > s->len) {
		CHECK(0, "consumed %u > buffered %u", io.consumed, s->len);
		return SDK_STATUS_INTERNAL_ERROR;
	}
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
	s->frames += io.frames;
	s->starved = io.starved;
	s->complete = io.complete;
	return status;
}

/* Feed `data` in `chunk` slices through a bounded input ring, like the
 * mailbox FEED path: compact, append, decode; EOF with the final slice. */
static uint16_t run_stream(struct sim *s, const uint8_t *data, uint32_t size,
                           uint32_t chunk, uint32_t in_cap, uint32_t pcm_cap,
                           uint32_t format, int drain_each)
{
	uint32_t fed = 0U;
	uint32_t guard = 0U;
	uint16_t status = SDK_STATUS_OK;

	memset(s, 0, sizeof(*s));
	s->in_cap = in_cap;
	s->pcm_cap = pcm_cap;
	s->pcm_budget = pcm_cap;
	sdk_flac_init(&s->st, format);
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
		} else if (fed < size) {
			return 0xFFFFU;   /* ring full without a decodable unit */
		}
		if (fed == size)
			s->eof = 1;
		status = sim_decode(s, drain_each && !s->eof);
		if (status != SDK_STATUS_OK)
			return status;
	}
	return status;
}

static uint32_t expected_pcm(const struct flac_fixture *fx, uint32_t cb,
                             uint8_t *dst)
{
	uint32_t w = (fx->bits + 7U) / 8U;
	uint32_t n = fx->ref_len / w;
	uint32_t i, k;

	for (i = 0U; i < n; i++) {
		int32_t v = 0;
		uint32_t u = 0U;

		for (k = 0U; k < w; k++)
			u = (u << 8) | fx->ref[i * w + k];
		if (u & (1U << (8U * w - 1U)))
			v = (int32_t)(u | ~((1U << (8U * w)) - 1U));
		else
			v = (int32_t)u;
		u = (uint32_t)v << (cb * 8U - fx->bits);
		for (k = 0U; k < cb; k++)
			dst[i * cb + k] = (uint8_t)(u >> (8U * (cb - 1U - k)));
	}
	return n * cb;
}

static uint8_t g_expect[MAX_OUT];
static uint8_t g_mut[256 * 1024];

static void check_clean(const char *what)
{
	CHECK(host_runtime_allocated_bytes() == 0U && host_runtime_live_blocks() == 0U &&
	      sdk_decode_tracked_count() == 0U,
	      "%s: leak bytes=%zu blocks=%u tracked=%u", what,
	      host_runtime_allocated_bytes(), host_runtime_live_blocks(),
	      sdk_decode_tracked_count());
}

static void test_fixtures_exact(void)
{
	static const uint32_t chunks[] = { 1U, 7U, 333U, 4096U, 1U << 20 };
	size_t f;
	unsigned c, fmt;

	for (f = 0; f < sizeof(flac_fixtures) / sizeof(flac_fixtures[0]); f++) {
		const struct flac_fixture *fx = &flac_fixtures[f];

		for (fmt = 0U; fmt < 2U; fmt++) {
			uint32_t format = fmt ? SDK_AUDIO_SAMPLE_FORMAT_S32BE :
			                        SDK_AUDIO_SAMPLE_FORMAT_S16BE;
			uint32_t cb = fmt ? 4U : 2U;
			uint32_t want;

			if (!fmt && fx->bits > 16U)
				continue;
			want = expected_pcm(fx, cb, g_expect);
			for (c = 0U; c < sizeof(chunks) / sizeof(chunks[0]); c++) {
				struct sim *s = &g_sim;
				uint16_t st;

				host_runtime_reset();
				st = run_stream(s, fx->flac, fx->flac_len, chunks[c],
				                16384U, 65536U, format, 0);
				CHECK(st == SDK_STATUS_OK && s->complete,
				      "%s fmt%u chunk %u: status %u complete %d",
				      fx->name, fmt, chunks[c], st, s->complete);
				CHECK(s->out_len == want && memcmp(s->out, g_expect, want) == 0,
				      "%s fmt%u chunk %u: pcm mismatch (%u vs %u bytes)",
				      fx->name, fmt, chunks[c], s->out_len, want);
				CHECK(s->st.sample_rate == fx->rate && s->st.channels == fx->channels &&
				      s->st.bits_per_sample == fx->bits,
				      "%s: geometry", fx->name);
				CHECK((s->st.total_samples == 0U) ==
				      (fx->name[strlen(fx->name) - 1] == 'u'),
				      "%s: known/unknown total", fx->name);
				if (c == 3U)
					printf("  %-10s %-5s peak tracked alloc: backend %zu B, heap %zu B\n",
					       fx->name, fmt ? "S32BE" : "S16BE",
					       s->st.alloc_peak,
					       host_runtime_peak_allocated_bytes());
				sdk_flac_release(&s->st);
				check_clean(fx->name);
			}
		}
	}
}

static uint16_t run_bytes(const uint8_t *data, uint32_t len, uint32_t format,
                          uint32_t pcm_cap)
{
	uint16_t st;

	host_runtime_reset();
	st = run_stream(&g_sim, data, len, 4096U, 16384U, pcm_cap, format, 0);
	sdk_flac_release(&g_sim.st);
	check_clean("run_bytes");
	return st;
}

static const struct flac_fixture *fixture(const char *name)
{
	size_t f;

	for (f = 0; f < sizeof(flac_fixtures) / sizeof(flac_fixtures[0]); f++)
		if (strcmp(flac_fixtures[f].name, name) == 0)
			return &flac_fixtures[f];
	return 0;
}

static void test_rejections(void)
{
	const struct flac_fixture *s16 = fixture("stereo16");
	const struct flac_fixture *m24 = fixture("mono24");
	const uint32_t S16 = SDK_AUDIO_SAMPLE_FORMAT_S16BE;
	const uint32_t S32 = SDK_AUDIO_SAMPLE_FORMAT_S32BE;
	uint16_t st;

	st = run_bytes(m24->flac, m24->flac_len, S16, 65536U);
	CHECK(st == SDK_STATUS_UNSUPPORTED, "24-bit as S16BE: %u", st);
	st = run_bytes(fx_multi3_flac, sizeof(fx_multi3_flac), S32, 65536U);
	CHECK(st == SDK_STATUS_UNSUPPORTED, "3 channels: %u", st);
	st = run_bytes(fx_ogg16_flac, sizeof(fx_ogg16_flac), S16, 65536U);
	CHECK(st == SDK_STATUS_UNSUPPORTED, "Ogg-FLAC: %u", st);
	CHECK(g_sim.out_len == 0U, "Ogg-FLAC produced PCM");

	/* PCM ring one byte short of the largest STREAMINFO frame. */
	st = run_bytes(s16->flac, s16->flac_len, S16, 1152U * 4U - 1U);
	CHECK(st == SDK_STATUS_UNSUPPORTED, "pcm ring < max frame: %u", st);
	st = run_bytes(s16->flac, s16->flac_len, S16, 1152U * 4U);
	CHECK(st == SDK_STATUS_OK, "pcm ring == max frame: %u", st);

	/* Invalid metadata. */
	memcpy(g_mut, s16->flac, s16->flac_len);
	g_mut[0] = 'X';
	CHECK(run_bytes(g_mut, s16->flac_len, S16, 65536U) == SDK_STATUS_IO_ERROR, "bad magic");
	memcpy(g_mut, s16->flac, s16->flac_len);
	g_mut[7] = 33;   /* STREAMINFO length */
	CHECK(run_bytes(g_mut, s16->flac_len, S16, 65536U) == SDK_STATUS_IO_ERROR, "streaminfo length");
	memcpy(g_mut, s16->flac, s16->flac_len);
	g_mut[4] = (uint8_t)((g_mut[4] & 0x80U) | 3U);   /* first block not STREAMINFO */
	CHECK(run_bytes(g_mut, s16->flac_len, S16, 65536U) == SDK_STATUS_IO_ERROR, "first block type");
	memcpy(g_mut, s16->flac, s16->flac_len);
	g_mut[8 + 2] = 0; g_mut[8 + 3] = 0;   /* max blocksize 0 */
	CHECK(run_bytes(g_mut, s16->flac_len, S16, 65536U) == SDK_STATUS_IO_ERROR, "max blocksize 0");
	/* Sample rate 4000 Hz (20-bit field at STREAMINFO byte 10). */
	memcpy(g_mut, s16->flac, s16->flac_len);
	g_mut[18] = (uint8_t)(4000U >> 12); g_mut[19] = (uint8_t)(4000U >> 4);
	g_mut[20] = (uint8_t)(((4000U & 15U) << 4) | (g_mut[20] & 15U));
	CHECK(run_bytes(g_mut, s16->flac_len, S16, 65536U) == SDK_STATUS_UNSUPPORTED, "rate 4000");
	/* 32-bit source (bps-1 = 31). */
	memcpy(g_mut, s16->flac, s16->flac_len);
	g_mut[20] = (uint8_t)((g_mut[20] & 0xFEU) | 1U);
	g_mut[21] = (uint8_t)((g_mut[21] & 0x0FU) | 0xF0U);
	CHECK(run_bytes(g_mut, s16->flac_len, S32, 65536U) == SDK_STATUS_UNSUPPORTED, "32-bit");
}

static void test_corruption(void)
{
	const struct flac_fixture *fx = fixture("stereo16");
	const uint32_t S16 = SDK_AUDIO_SAMPLE_FORMAT_S16BE;
	uint32_t want = expected_pcm(fx, 2U, g_expect);
	uint16_t st;

	/* CRC error in the final frame. */
	memcpy(g_mut, fx->flac, fx->flac_len);
	g_mut[fx->flac_len - 40U] ^= 0x10U;
	st = run_bytes(g_mut, fx->flac_len, S16, 65536U);
	CHECK(st == SDK_STATUS_IO_ERROR, "crc error: %u", st);
	CHECK(g_sim.out_len < want && memcmp(g_sim.out, g_expect, g_sim.out_len) == 0,
	      "crc error: published PCM must be an exact prefix");
	/* CRC error in a middle frame, fed byte-by-byte. */
	memcpy(g_mut, fx->flac, fx->flac_len);
	g_mut[fx->flac_len / 2U] ^= 0x01U;
	host_runtime_reset();
	st = run_stream(&g_sim, g_mut, fx->flac_len, 1U, 16384U, 65536U, S16, 0);
	CHECK(st == SDK_STATUS_IO_ERROR, "mid crc error: %u", st);
	sdk_flac_release(&g_sim.st);
	check_clean("mid crc");
	/* Truncated final frame. */
	st = run_bytes(fx->flac, fx->flac_len - 50U, S16, 65536U);
	CHECK(st == SDK_STATUS_IO_ERROR, "truncated frame: %u", st);
	/* Truncated at an exact frame boundary: STREAMINFO total says more. */
	{
		const struct flac_fixture *m8 = fixture("mono8");
		uint32_t i, cut = 0U;

		for (i = 100U; i + 1U < m8->flac_len; i++)
			if (m8->flac[i] == 0xFF && (m8->flac[i + 1U] & 0xFE) == 0xF8)
				cut = i;   /* last frame start */
		st = run_bytes(m8->flac, cut, SDK_AUDIO_SAMPLE_FORMAT_S16BE, 65536U);
		CHECK(st == SDK_STATUS_IO_ERROR, "missing final frame: %u", st);
	}
	/* Truncated inside metadata. */
	st = run_bytes(fx->flac, 60U, S16, 65536U);
	CHECK(st == SDK_STATUS_IO_ERROR, "truncated metadata: %u", st);
	/* Status is sticky. */
	{
		struct sdk_audio_codec_io io;

		memset(&io, 0, sizeof(io));
		io.pcm = g_sim.pcm;
		io.pcm_capacity = 1024U;
		CHECK(sdk_flac_decode(&g_sim.st, &io) == SDK_STATUS_IO_ERROR, "sticky status");
	}
}

static void test_drain_and_budget(void)
{
	const struct flac_fixture *fx = fixture("stereo16");
	uint32_t want = expected_pcm(fx, 2U, g_expect);
	struct sim *s = &g_sim;
	uint16_t st;

	/* DRAIN on every feed: complete frames decode early, tails retained. */
	host_runtime_reset();
	st = run_stream(s, fx->flac, fx->flac_len, 997U, 16384U, 65536U,
	                SDK_AUDIO_SAMPLE_FORMAT_S16BE, 1);
	CHECK(st == SDK_STATUS_OK && s->out_len == want &&
	      memcmp(s->out, g_expect, want) == 0, "drain feeds: %u", st);
	sdk_flac_release(&s->st);
	check_clean("drain");

	/* Starvation: a partial frame without EOF decodes nothing more. */
	host_runtime_reset();
	memset(s, 0, sizeof(*s));
	s->in_cap = MAX_IN_CAP;
	s->pcm_cap = 65536U;
	s->pcm_budget = 1U;   /* at most one frame per call */
	sdk_flac_init(&s->st, SDK_AUDIO_SAMPLE_FORMAT_S16BE);
	memcpy(s->in, fx->flac, fx->flac_len - 10U);
	s->len = fx->flac_len - 10U;
	st = sim_decode(s, 0);
	CHECK(st == SDK_STATUS_OK && s->frames == 1U, "budget: one frame per call (%u)", s->frames);
	while (st == SDK_STATUS_OK && s->frames < 64U) {
		uint32_t before = s->frames;

		st = sim_decode(s, 0);
		if (s->frames == before)
			break;
	}
	CHECK(st == SDK_STATUS_OK && s->starved && s->out_len < want,
	      "starved on partial final frame");
	memcpy(s->in + s->off + s->len, fx->flac + fx->flac_len - 10U, 10U);
	s->len += 10U;
	s->eof = 1;
	s->pcm_budget = 65536U;
	st = sim_decode(s, 0);
	CHECK(st == SDK_STATUS_OK && s->complete && s->out_len == want &&
	      memcmp(s->out, g_expect, want) == 0, "resume after starvation");
	sdk_flac_release(&s->st);
	check_clean("budget");
}

static void test_allocation_failures(void)
{
	const struct flac_fixture *fx = fixture("stereo16");
	unsigned attempts, n, nomem = 0U;
	uint16_t st;

	host_runtime_reset();
	st = run_stream(&g_sim, fx->flac, fx->flac_len, 4096U, 16384U, 65536U,
	                SDK_AUDIO_SAMPLE_FORMAT_S16BE, 0);
	attempts = host_runtime_allocation_attempts();
	sdk_flac_release(&g_sim.st);
	check_clean("alloc baseline");
	CHECK(st == SDK_STATUS_OK && attempts > 0U, "alloc baseline");
	for (n = 1U; n <= attempts; n++) {
		host_runtime_reset();
		host_runtime_fail_on_allocation(n);
		st = run_stream(&g_sim, fx->flac, fx->flac_len, 4096U, 16384U, 65536U,
		                SDK_AUDIO_SAMPLE_FORMAT_S16BE, 0);
		CHECK(st == SDK_STATUS_NO_MEMORY, "fail alloc %u -> %u", n, st);
		if (st == SDK_STATUS_NO_MEMORY)
			nomem++;
		sdk_flac_release(&g_sim.st);
		check_clean("alloc failure");
	}
	printf("  allocation failure injection: %u/%u points -> NO_MEMORY, no leaks\n",
	       nomem, attempts);
}

static void test_restart_reclaim(void)
{
	const struct flac_fixture *fx = fixture("stereo24u");
	unsigned reclaimed;

	host_runtime_reset();
	memset(&g_sim, 0, sizeof(g_sim));
	g_sim.in_cap = MAX_IN_CAP;
	g_sim.pcm_cap = 65536U;
	g_sim.pcm_budget = 65536U;
	memcpy(g_sim.in, fx->flac, fx->flac_len * 3U / 4U);
	g_sim.len = fx->flac_len * 3U / 4U;
	CHECK(sim_decode(&g_sim, 0) == SDK_STATUS_OK && g_sim.frames > 0U, "partial decode");
	CHECK(host_runtime_live_blocks() > 0U, "decoder holds blocks");
	/* Core-1 cold restart: core 0 frees every tracked block. */
	reclaimed = sdk_decode_reclaim(sdk_decode_heap_free);
	sdk_flac_forget(&g_sim.st);
	CHECK(reclaimed > 0U, "reclaim freed nothing");
	check_clean("restart reclaim");
	printf("  restart reclaim: %u tracked blocks freed\n", reclaimed);
}

int main(void)
{
	test_fixtures_exact();
	test_rejections();
	test_corruption();
	test_drain_and_budget();
	test_allocation_failures();
	test_restart_reclaim();
	if (failures) {
		printf("flac_backend_test: %d failure(s)\n", failures);
		return 1;
	}
	printf("flac_backend_test: all passed\n");
	return 0;
}
