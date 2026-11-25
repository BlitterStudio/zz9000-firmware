/*
 * The cubic reader never reads outside the four taps of a frame it mixes.
 *
 * The sample bank is one page between two PROT_NONE pages, and the sample
 * is flush with the end of the page (forward play) or with its start
 * (backward play), so the last frame of the pass sits right at a protected
 * page. A read of a lane that mixes no frame, such as the unused lanes of
 * the final partial block of a vector run, is then a fault, and with it the
 * test fails with the scenario that did it, even where the PCM would not
 * change. The scenarios sweep the four sample formats, both directions, the
 * rates up to the maximum (a large step puts an unused lane far outside the
 * sample), sample lengths that leave every remainder of 1, 2 and 3 frames
 * for the final block of the interior (the remainder is worked out here
 * from the documented run split, only to prove the sweep reaches each
 * one), with and without a ramp, a loop and a silent side.
 *
 * Linux only (mmap, mprotect, SIGSEGV); elsewhere it reports a skip.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1
#endif

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if defined(__linux__)
#include <setjmp.h>
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>

#include "audio_voice.h"

static int failures;

#define CHECK(cond, ...) do { \
	if (!(cond)) { \
		failures++; \
		printf("FAIL %s:%d: ", __FILE__, __LINE__); \
		printf(__VA_ARGS__); \
		printf("\n"); \
	} \
} while (0)

static struct audio_voice_cmd mk(uint32_t stamp, uint8_t voice, uint8_t op,
                                 uint16_t flags, uint32_t a0, uint32_t a1)
{
	struct audio_voice_cmd c;

	c.stamp = stamp;
	c.voice = voice;
	c.op = op;
	c.flags = flags;
	c.arg0 = a0;
	c.arg1 = a1;
	return c;
}

static sigjmp_buf g_jump;
static void *volatile g_fault_addr;

static void on_segv(int sig, siginfo_t *si, void *ctx)
{
	(void)sig;
	(void)ctx;
	g_fault_addr = si->si_addr;
	siglongjmp(g_jump, 1);
}

static struct audio_voice_engine g_engine;
static int16_t g_out[2 * 512];
static struct audio_voice_cmd g_cmds[8];

static const char *const fmt_name[4] = { "S8", "S16", "S8S", "S16S" };
static const uint32_t fmt_shift[4] = { 0, 1, 1, 2 };

static uint64_t ceil_div(uint64_t a, uint64_t b)
{
	return (a + b - 1U) / b;
}

/*
 * The frames of the final vector block of the interior of a no-loop, no-ramp
 * pass that starts at the first frame (forward) or the last (backward): the
 * interior is the frames whose four taps are neighbours in the sample, and
 * the engine mixes them four at a time.
 */
static uint32_t interior_rem(int back, uint32_t len, uint32_t step)
{
	uint64_t run, ka, kb;

	if (!back) {
		const uint64_t end = (uint64_t)len << 16;
		const uint64_t hi = (uint64_t)(len >= 3U ? len - 2U : 0U) << 16;

		run = ceil_div(end, step);
		ka = ceil_div(1U << 16, step);
		kb = ceil_div(hi, step);
	} else {
		const uint64_t p0 = (uint64_t)(len - 1U) << 16;
		const uint64_t low2 = 2U << 16;

		run = p0 / step + 1U;
		ka = 1U;
		kb = p0 < low2 ? 0U : (p0 - low2) / step + 1U;
	}
	if (ka > run)
		ka = run;
	if (kb > run)
		kb = run;
	if (kb < ka)
		kb = ka;
	return (uint32_t)((kb - ka) & 3U);
}

struct scenario {
	uint32_t fmt, back, hz, len, ramp, loop, vol_l, vol_r;
};

/* Returns 0, or 1 when the engine touched a protected page. */
static int run_scenario(const struct scenario *s, uint8_t *bank,
                        uint32_t page, int *sounded)
{
	const uint32_t shift = fmt_shift[s->fmt];
	const uint32_t bytes = s->len << shift;
	const uint32_t off = s->back ? 0U : page - bytes;
	size_t n = 0, i;

	if (sigsetjmp(g_jump, 1) != 0)
		return 1;
	audio_voice_init(&g_engine, bank, page, 3546895U);
	if (audio_voice_sample_set(&g_engine, 0, off, bytes, s->fmt) != 0) {
		printf("FAIL sample_set\n");
		failures++;
		return 0;
	}
	if (s->ramp)
		g_cmds[n++] = mk(0, 0, AUDIO_VOICE_OP_CONFIG,
		                 AUDIO_VOICE_CONFIG_RAMP, 64U, 0);
	g_cmds[n++] = mk(0, 0, AUDIO_VOICE_OP_MODE, AUDIO_VOICE_MODE_INTERP,
	                 AUDIO_VOICE_INTERP_MODE_CUBIC, 0);
	g_cmds[n++] = mk(0, 0, AUDIO_VOICE_OP_SET, AUDIO_VOICE_SET_RATE |
	                 AUDIO_VOICE_RATE_HZ | AUDIO_VOICE_SET_VOLUME, s->hz,
	                 (s->vol_l << 16) | s->vol_r);
	g_cmds[n++] = mk(0, 0, AUDIO_VOICE_OP_START, 0,
	                 s->back ? (s->len - 1U) << shift : 0U, 0);
	if (s->loop)
		g_cmds[n++] = mk(0, 0, AUDIO_VOICE_OP_LOOP, 0, 0, bytes);
	if (s->back)
		g_cmds[n++] = mk(0, 0, AUDIO_VOICE_OP_DIRECTION,
		                 AUDIO_VOICE_DIR_BACKWARD, 0, 0);
	memset(g_out, 0, sizeof(g_out));
	audio_voice_render(&g_engine, g_out, 400U, g_cmds, n);
	for (i = 0; i < 2U * 400U; i++) {
		if (g_out[i] != 0) {
			*sounded = 1;
			break;
		}
	}
	return 0;
}

int main(void)
{
	static const uint32_t rates[] = { 6000U, 12000U, 24000U, 36000U, 48000U,
	                                  72000U, 96000U, 144000U, 192000U };
	static const uint32_t vols[3][2] = { { 40000U, 30000U },
	                                     { 40000U, 0U }, { 0U, 30000U } };
	const long ps = sysconf(_SC_PAGESIZE);
	const uint32_t page = ps > 0 ? (uint32_t)ps : 4096U;
	uint8_t *map;
	uint8_t *bank;
	struct sigaction sa;
	uint32_t seen[4][2][4];
	uint32_t seed = 0x1234567U, i;
	uint64_t runs = 0, faults = 0;
	uint32_t fmt, back, r;

	map = mmap(NULL, 3U * page, PROT_READ | PROT_WRITE,
	           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (map == MAP_FAILED) {
		printf("audio_voice_cubic_guard_test: mmap failed, skipped\n");
		return 0;
	}
	bank = map + page;
	for (i = 0; i < page; i++) {
		seed = seed * 1664525U + 1013904223U;
		bank[i] = (uint8_t)(seed >> 24);
	}
	if (mprotect(map, page, PROT_NONE) != 0 ||
	    mprotect(map + 2U * page, page, PROT_NONE) != 0) {
		printf("audio_voice_cubic_guard_test: mprotect failed, skipped\n");
		return 0;
	}
	memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = on_segv;
	sa.sa_flags = SA_SIGINFO | SA_NODEFER;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGSEGV, &sa, NULL);
	sigaction(SIGBUS, &sa, NULL);
	memset(seen, 0, sizeof(seen));

	for (fmt = 0; fmt < 4U; fmt++) {
		for (back = 0; back < 2U; back++) {
			uint32_t h, len, ramp, loop, v;

			for (h = 0; h < sizeof(rates) / sizeof(rates[0]); h++) {
				const uint32_t step =
					(uint32_t)(((uint64_t)rates[h] << 16) / 48000U);

				for (len = 6; len <= 45U; len++) {
					for (ramp = 0; ramp < 2U; ramp++) {
						for (loop = 0; loop < 2U; loop++) {
							for (v = 0; v < 3U; v++) {
								struct scenario s;
								int sounded = 0;

								s.fmt = fmt;
								s.back = back;
								s.hz = rates[h];
								s.len = len;
								s.ramp = ramp;
								s.loop = loop;
								s.vol_l = vols[v][0];
								s.vol_r = vols[v][1];
								runs++;
								if (run_scenario(&s, bank, page,
								                 &sounded)) {
									faults++;
									CHECK(0, "%s %s %u Hz len %u ramp %u "
									      "loop %u vol %u/%u: read at %p "
									      "outside the bank (bank %p..%p)",
									      fmt_name[fmt],
									      back ? "backward" : "forward",
									      s.hz, len, ramp, loop, s.vol_l,
									      s.vol_r, g_fault_addr,
									      (void *)bank,
									      (void *)(bank + page));
								} else if (v == 0) {
									CHECK(sounded, "%s %s %u Hz len %u: "
									      "silent", fmt_name[fmt],
									      back ? "backward" : "forward",
									      s.hz, len);
								}
								if (!ramp && !loop && v == 0)
									seen[fmt][back]
									    [interior_rem(back, len, step)]++;
							}
						}
					}
				}
			}
		}
	}
	for (fmt = 0; fmt < 4U; fmt++)
		for (back = 0; back < 2U; back++)
			for (r = 1; r < 4U; r++)
				CHECK(seen[fmt][back][r] >= 8U,
				      "%s %s: only %u scenarios end the interior with a "
				      "block of %u frames", fmt_name[fmt],
				      back ? "backward" : "forward", seen[fmt][back][r], r);
	if (failures != 0) {
		printf("audio_voice_cubic_guard_test: %d failures in %llu scenarios "
		       "(%llu faulted)\n", failures, (unsigned long long)runs,
		       (unsigned long long)faults);
		return 1;
	}
	printf("audio_voice_cubic_guard_test: %llu scenarios, 4 formats, both "
	       "directions, final blocks of 1, 2 and 3 frames, no read outside "
	       "the bank\n", (unsigned long long)runs);
	return 0;
}
#else
int main(void)
{
	printf("audio_voice_cubic_guard_test: skipped (needs Linux mmap)\n");
	return 0;
}
#endif
