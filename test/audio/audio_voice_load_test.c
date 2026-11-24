/*
 * The record schedule of `zz9k-voiceplay LOAD CHURN ENV` (sdk/tools/
 * zz9k-voice-load.h): restarts of voice groups and the key-off of every
 * note, in the order the ring needs.
 *
 * The reference enumerates, for every voice, its start frames and the
 * key-off frames of its notes straight from the rule (a voice restarts at
 * every slot n with n modulo groups equal to its own, a key-off follows each
 * note by the hold) and compares them with what the schedule wrote. The
 * schedule's records then drive the real engine, which must take them all
 * without a late or rejected record and end exactly the releases that finish
 * before the next restart of their voice.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_voice.h"
#include "zz9k-voice-load.h"

static int failures;

#define CHECK(cond, ...) do { \
	if (!(cond)) { \
		failures++; \
		printf("FAIL %s:%d: ", __FILE__, __LINE__); \
		printf(__VA_ARGS__); \
		printf("\n"); \
	} \
} while (0)

#define LEAD 7680U
#define MAX_REC 20000U

static ZZ9KVoiceRecord g_rec[MAX_REC];

/* Writes slots until the next one is at or past `until`. Returns the record
 * count; *largest is the biggest batch of one slot. */
static uint32_t generate(ZZ9KVoiceLoad *l, uint32_t until, uint32_t *largest)
{
	uint32_t n = 0, got;

	*largest = 0;
	while (zz9k_voice_load_next(l, NULL) < until) {
		got = zz9k_voice_load_emit(l, LEAD, g_rec + n);
		if (got > *largest)
			*largest = got;
		n += got;
		if (n + ZZ9K_VOICE_LOAD_MAX_RECORDS > MAX_REC) {
			CHECK(0, "the test buffer is too small");
			break;
		}
	}
	return n;
}

/* The records of voice v of the schedule, in order, as (stamp, op, flags). */
struct ev {
	uint32_t frame;
	uint8_t op;
	uint16_t flags;
};

static uint32_t voice_events(uint32_t n, uint32_t v, struct ev *out)
{
	uint32_t i, k = 0;

	for (i = 0; i < n; i++) {
		if (g_rec[i].voice != v || g_rec[i].op == ZZ9K_VOICE_OP_LOOP)
			continue;
		out[k].frame = g_rec[i].stamp - LEAD;
		out[k].op = g_rec[i].op;
		out[k].flags = g_rec[i].flags;
		k++;
	}
	return k;
}

/* The reference: starts of voice v below `until` as the rule gives them
 * (frame 0 is the tool's own start), then the key-offs, merged by frame with
 * the key-off first on a tie. */
static uint32_t reference(uint32_t voices, uint32_t groups, uint32_t step,
                          uint32_t hold, uint32_t until, uint32_t v,
                          struct ev *out)
{
	static uint32_t starts[4096];
	uint32_t ns = 0, i = 0, j = 0, k = 0, n;
	uint32_t keys[4096], nk = 0;

	(void)voices;
	starts[ns++] = 0;
	for (n = 1; n * step < until; n++)
		if (n % groups == v % groups)
			starts[ns++] = n * step;
	for (i = 0; i < ns; i++) {
		const uint32_t next = i + 1 < ns ? starts[i + 1] : 0xFFFFFFFFU;

		if (hold != 0 && hold < next - starts[i] &&
		    starts[i] + hold < until)
			keys[nk++] = starts[i] + hold;
	}
	/* the initial start is not a record of the schedule */
	i = 1;
	while (i < ns || j < nk) {
		if (j < nk && (i >= ns || keys[j] <= starts[i])) {
			out[k].frame = keys[j++];
			out[k].op = ZZ9K_VOICE_OP_STOP;
			out[k].flags = ZZ9K_VOICE_STOP_RELEASE;
		} else {
			out[k].frame = starts[i++];
			out[k].op = ZZ9K_VOICE_OP_START;
			out[k].flags = 0;
		}
		k++;
	}
	return k;
}

static void run_config(uint32_t voices, uint32_t groups, uint32_t step,
                       uint32_t hold)
{
	static struct ev got[4096], want[4096];
	ZZ9KVoiceLoad l;
	const uint32_t until = 12U * groups * step;
	uint32_t largest, n, v, i, restarts = 0, keyoffs = 0;
	int ordered = 1;

	zz9k_voice_load_init(&l, voices, groups, step, hold, 64U);
	n = generate(&l, until, &largest);
	for (i = 1; i < n; i++)
		if (g_rec[i].stamp < g_rec[i - 1].stamp)
			ordered = 0;
	CHECK(ordered, "voices %u groups %u step %u hold %u: stamps never "
	      "decrease", voices, groups, step, hold);
	CHECK(largest <= zz9k_voice_load_room(&l) &&
	      largest <= ZZ9K_VOICE_LOAD_MAX_RECORDS,
	      "voices %u groups %u hold %u: a slot writes %u records, room %u",
	      voices, groups, hold, largest, zz9k_voice_load_room(&l));
	for (v = 0; v < voices; v++) {
		const uint32_t ng = voice_events(n, v, got);
		const uint32_t nw = reference(voices, groups, step, hold, until, v,
		                              want);
		uint32_t k;

		/* the schedule writes every slot below `until`, so the lists are
		 * the same */
		CHECK(ng == nw, "voices %u groups %u step %u hold %u voice %u: %u "
		      "events, reference %u", voices, groups, step, hold, v, ng, nw);
		for (k = 0; k < ng && k < nw; k++) {
			if (got[k].frame != want[k].frame || got[k].op != want[k].op ||
			    got[k].flags != want[k].flags) {
				CHECK(0, "voices %u groups %u step %u hold %u voice %u "
				      "event %u: frame %u op %u flags %u, want %u %u %u",
				      voices, groups, step, hold, v, k, got[k].frame,
				      got[k].op, got[k].flags, want[k].frame, want[k].op,
				      want[k].flags);
				return;
			}
			if (got[k].op == ZZ9K_VOICE_OP_START)
				restarts++;
			else
				keyoffs++;
		}
	}
	CHECK(l.restarts == restarts && l.keyoffs == keyoffs && n > 0,
	      "counters (%u/%u restarts, %u/%u key-offs)", l.restarts, restarts,
	      l.keyoffs, keyoffs);
	if (hold == 0)
		CHECK(l.keyoffs == 0, "no hold, no key-offs (%u)", l.keyoffs);
}

/* On a tie the key-off comes before the start of another group. */
static void test_tie_order(void)
{
	ZZ9KVoiceLoad l;
	uint32_t n, largest, i, first_off = 0xFFFFFFFFU, first_start_after = 0;

	/* step 8, hold 24: key-off slot 0 is at frame 24, start slot 3 too */
	zz9k_voice_load_init(&l, 32U, 32U, 8U, 24U, 64U);
	n = generate(&l, 64U, &largest);
	for (i = 0; i < n; i++) {
		if (g_rec[i].stamp == LEAD + 24U &&
		    g_rec[i].op == ZZ9K_VOICE_OP_STOP && first_off == 0xFFFFFFFFU)
			first_off = i;
		if (g_rec[i].stamp == LEAD + 24U &&
		    g_rec[i].op == ZZ9K_VOICE_OP_START && !first_start_after)
			first_start_after = i;
	}
	CHECK(first_off != 0xFFFFFFFFU && first_start_after &&
	      first_off < first_start_after,
	      "the key-off of slot 0 precedes the start of slot 3 at frame 24 "
	      "(%u, %u)", first_off, first_start_after);
}

/* A hold at or beyond the period is refused; one below is not. */
static void test_hold_fit(void)
{
	ZZ9KVoiceLoad l;

	zz9k_voice_load_init(&l, 32U, 32U, 8U, 255U, 64U);
	CHECK(zz9k_voice_load_period(&l) == 256U && zz9k_voice_load_hold_ok(&l),
	      "255 of 256 fits");
	l.hold = 256U;
	CHECK(!zz9k_voice_load_hold_ok(&l), "256 of 256 does not fit");
	l.hold = 0U;
	CHECK(zz9k_voice_load_hold_ok(&l), "no hold always fits");
}

/* The ring a CHURN run with ENV asks for grants 512 records, the other 256,
 * and one extra release record per note needs the larger one. */
static void test_ring_request(void)
{
	CHECK(zz9k_voice_ring_records_for_bytes(8192U) == 256U,
	      "8192 bytes grant 256 records");
	CHECK(zz9k_voice_ring_records_for_bytes(8208U) == 512U,
	      "8208 bytes grant 512 records");
	CHECK(zz9k_voice_ring_records_for_bytes(8207U) == 256U,
	      "8207 bytes still grant 256");
	/* three records a voice and restart, 32 voices: 96 a slot or batch
	 * of slots, within one pending queue of 256 */
	CHECK(3U * ZZ9K_VOICE_MAX_VOICES <= 256U, "a period of notes fits the "
	      "pending queue");
}

/* The schedule drives the engine with 32 voices, ramp 256, an envelope with
 * a release of release_ms and the default hold of three quarters of the
 * period: the records are taken in order, none late or rejected, and the
 * voices end exactly the releases that finish before their next restart. */
static void test_engine(uint32_t groups, uint32_t release_ms)
{
	static struct audio_voice_engine e;
	static uint8_t bank[128];
	static int16_t out[2 * 960];
	static struct audio_voice_cmd cmds[MAX_REC];
	const uint32_t voices = 32U, ramp = 256U, step = 256U / groups;
	const uint32_t period = groups * step;
	const uint32_t hold = period * 3U / 4U, until = 40U * 256U;
	ZZ9KVoiceLoad l;
	uint32_t n = 0, v, i, k, frames = 0, used = 0, ends_want = 0;
	ZZ9KVoiceRecord *rec = g_rec;

	for (i = 0; i < sizeof(bank); i += 2) {
		bank[i] = 0x20;
		bank[i + 1] = (uint8_t)(i & 0x7F);
	}
	audio_voice_init(&e, bank, sizeof(bank), AUDIO_VOICE_CLOCK_PAL);
	audio_voice_sample_set(&e, 0, 0, sizeof(bank), AUDIO_VOICE_FMT_S16);
	zz9k_voice_build_config_ramp(&rec[n++], 0, ramp);
	for (v = 0; v < voices; v++) {
		zz9k_voice_build_envelope(&rec[n++], 0, (uint8_t)v, 1U, 1U, 8192U,
		                          (uint16_t)release_ms);
		zz9k_voice_build_set(&rec[n++], 0, (uint8_t)v,
		                     ZZ9K_VOICE_SET_RATE | ZZ9K_VOICE_SET_RATE_HZ |
		                     ZZ9K_VOICE_SET_VOLUME, 22050U, 3000U, 3000U);
		zz9k_voice_build_start(&rec[n++], 0, (uint8_t)v, 0U, 0U, 0U);
		zz9k_voice_build_loop(&rec[n++], 0, (uint8_t)v, 0U, sizeof(bank));
	}
	zz9k_voice_load_init(&l, voices, groups, step, hold, sizeof(bank));
	while (zz9k_voice_load_next(&l, NULL) < until)
		n += zz9k_voice_load_emit(&l, 0, rec + n);
	for (i = 0; i < n; i++) {
		cmds[i].stamp = rec[i].stamp;
		cmds[i].voice = rec[i].voice;
		cmds[i].op = rec[i].op;
		cmds[i].flags = rec[i].flags;
		cmds[i].arg0 = rec[i].arg0;
		cmds[i].arg1 = rec[i].arg1;
	}
	while (frames < until + 960U) {
		used += audio_voice_render(&e, out, 960U, cmds + used, n - used);
		frames += 960U;
	}
	CHECK(used == n && e.late == 0 && e.rejected == 0,
	      "groups %u: used %u of %u, late %u rejected %u", groups, used, n,
	      e.late, e.rejected);
	/* A key-off ends its note after the release unless the voice restarts
	 * first. The first note of a voice has no key-off when its voice
	 * restarts before it is due. A tail's end is never counted. */
	for (v = 0; v < voices; v++) {
		uint32_t starts[512], ns = 0;

		starts[ns++] = 0;
		for (k = 1; k * step < until; k++)
			if (k % groups == v % groups)
				starts[ns++] = k * step;
		for (i = 0; i < ns; i++) {
			const uint32_t off = starts[i] + hold;
			const uint32_t end = off + 48U * release_ms;
			const uint32_t next = i + 1 < ns ? starts[i + 1] : 0xFFFFFFFFU;

			if (hold < next - starts[i] && off < until && end <= next &&
			    end <= frames)
				ends_want++;
		}
	}
	CHECK(e.ends == ends_want, "groups %u release %u ms: %u ends, expected %u",
	      groups, release_ms, e.ends, ends_want);
}

int main(void)
{
	static const uint32_t voices[] = { 32, 32, 32, 32, 8, 1, 5, 32 };
	static const uint32_t groups[] = { 32, 1, 8, 2, 4, 1, 3, 5 };
	static const uint32_t steps[] = { 8, 256, 32, 128, 64, 256, 85, 51 };
	uint32_t i;

	for (i = 0; i < sizeof(voices) / sizeof(voices[0]); i++) {
		const uint32_t period = groups[i] * steps[i];
		uint32_t holds[5];
		uint32_t h;

		holds[0] = 0;
		holds[1] = 1;
		holds[2] = steps[i];
		holds[3] = period * 3U / 4U;
		holds[4] = period - 1U;
		for (h = 0; h < 5; h++)
			if (holds[h] < period)
				run_config(voices[i], groups[i], steps[i], holds[h]);
	}
	test_tie_order();
	test_hold_fit();
	test_ring_request();
	test_engine(32U, 1U);
	test_engine(8U, 1U);
	test_engine(1U, 1U);
	test_engine(32U, 8U);
	if (failures) {
		printf("audio_voice_load_test: %d FAILURES\n", failures);
		return 1;
	}
	printf("audio_voice_load_test: the CHURN and ENV schedule matches the "
	       "rule for 8 shapes and 5 holds, ties, the hold limit, the ring "
	       "request, and the engine takes it\n");
	return 0;
}
