/*
 * Reference test for the voice engine against libpt4.
 *
 * Each module plays twice, frame by frame. libpt4 renders it through its
 * WIDE path (every voice on its own, as on a wavetable card). The voice
 * engine renders it from the voice events that zz9k-voice-pt4 takes from a
 * second libpt4 instance, with libpt4's sample arena as the bank. After
 * every frame the test compares each voice's activity, sample position and
 * fraction, which must be equal, and the mixed output.
 *
 * The output may differ by rounding only. libpt4 sums sample * volume
 * (0..64) per side and divides the sum by 8 (4 / 32 voices); the engine
 * rounds each voice down on its own, so a side may be off by at most one
 * per voice on that side plus one for libpt4's final truncation. The
 * oracle passes each voice's volume as volume * 1024 / voices, which makes
 * the engine's per-voice term exactly sample * volume * 4 / voices.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_voice.h"
#include "zz9k-voice-pt4.h"
#include "libpt4/src/pt4_internal.h"

static int failures;

#define CHECK(cond, ...) do { \
	if (!(cond)) { \
		failures++; \
		printf("FAIL %s:%d: ", __FILE__, __LINE__); \
		printf(__VA_ARGS__); \
		printf("\n"); \
	} \
} while (0)

#define MAX_CMDS 4096U
#define SECONDS 30U

struct cmd_queue {
	struct audio_voice_cmd cmd[MAX_CMDS];
	size_t head;
	size_t count;
	int voices;
	int overflow;
};

static uint8_t *load_file(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");
	uint8_t *data;
	long size;

	if (f == NULL)
		return NULL;
	if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) <= 0 ||
	    fseek(f, 0, SEEK_SET) != 0) {
		fclose(f);
		return NULL;
	}
	data = malloc((size_t)size);
	if (data != NULL && fread(data, 1, (size_t)size, f) != (size_t)size) {
		free(data);
		data = NULL;
	}
	fclose(f);
	*len = (size_t)size;
	return data;
}

static int on_left(int voice)
{
	return (voice & 3) == 0 || (voice & 3) == 3;
}

static void push(void *user, const ZZ9KPt4Event *ev)
{
	struct cmd_queue *q = user;
	struct audio_voice_cmd c;
	uint32_t vol;

	memset(&c, 0, sizeof(c));
	c.stamp = ev->frame;
	c.voice = ev->voice;
	switch (ev->kind) {
	case ZZ9K_PT4_EV_START:
		c.op = AUDIO_VOICE_OP_START;
		c.flags = 0; /* sample 0: the whole arena */
		c.arg0 = ev->a;
		c.arg1 = ev->b;
		break;
	case ZZ9K_PT4_EV_STOP:
		c.op = AUDIO_VOICE_OP_STOP;
		break;
	case ZZ9K_PT4_EV_LOOP:
		c.op = AUDIO_VOICE_OP_LOOP;
		c.arg0 = ev->a;
		c.arg1 = ev->b;
		break;
	case ZZ9K_PT4_EV_DIRECTION:
		c.op = AUDIO_VOICE_OP_DIRECTION;
		c.flags = ev->a ? AUDIO_VOICE_DIR_BACKWARD : 0;
		break;
	case ZZ9K_PT4_EV_PERIOD:
		c.op = AUDIO_VOICE_OP_SET;
		c.flags = AUDIO_VOICE_SET_RATE;
		c.arg0 = ev->a;
		break;
	case ZZ9K_PT4_EV_VOLUME:
		c.op = AUDIO_VOICE_OP_SET;
		c.flags = AUDIO_VOICE_SET_VOLUME;
		vol = ev->a * 1024U / (uint32_t)q->voices;
		c.arg1 = on_left(ev->voice) ? (vol << 16) : vol;
		break;
	default:
		CHECK(0, "unknown event kind %u", ev->kind);
		return;
	}
	if (q->head + q->count >= MAX_CMDS) {
		memmove(q->cmd, q->cmd + q->head, q->count * sizeof(q->cmd[0]));
		q->head = 0;
	}
	if (q->count >= MAX_CMDS) {
		q->overflow = 1;
		return;
	}
	q->cmd[q->head + q->count++] = c;
}

static pt4_song *open_song(const uint8_t *data, size_t len)
{
	int err = 0;
	pt4_song *s = pt4_open(data, len, NULL, &err);

	CHECK(s != NULL, "pt4_open failed: %d", err);
	return s;
}

static void compare_module(const char *name)
{
	char path[256];
	uint8_t *data;
	size_t len = 0;
	pt4_song *ref, *drv;
	ZZ9KPt4Events ref_cfg, ev;
	static struct cmd_queue q;
	static struct audio_voice_engine e;
	const uint8_t *bank;
	uint32_t bank_len;
	uint32_t frame, tick_left = 0, limit;
	uint32_t pos_mismatch = 0, pcm_mismatch = 0, max_diff = 0;
	uint32_t starts = 0;
	int ended = 0;
	int k;

	snprintf(path, sizeof(path), "fixtures/pt4/%s.mod", name);
	data = load_file(path, &len);
	CHECK(data != NULL, "%s: cannot read %s", name, path);
	if (data == NULL)
		return;
	ref = open_song(data, len);
	drv = open_song(data, len);
	if (ref == NULL || drv == NULL)
		goto out;
	/* The reference gets the same WIDE path, rate and start position;
	 * its events are never read. */
	CHECK(zz9k_pt4_events_init(&ref_cfg, ref, 1) == PT4_OK,
	      "%s: reference setup", name);
	CHECK(zz9k_pt4_events_init(&ev, drv, 1) == PT4_OK,
	      "%s: event setup", name);

	memset(&q, 0, sizeof(q));
	q.voices = zz9k_pt4_events_voices(&ev);
	bank = zz9k_pt4_events_bank(&ev, &bank_len);
	audio_voice_init(&e, bank, bank_len, AUDIO_VOICE_CLOCK_PAL);
	CHECK(audio_voice_sample_set(&e, 0, 0, bank_len,
	                             AUDIO_VOICE_FMT_S8) == 0,
	      "%s: bank sample", name);

	limit = SECONDS * ZZ9K_PT4_RATE;
	for (frame = 0; frame < limit && !ended; frame++) {
		int16_t ref_out[2], out[2];
		size_t used;
		int left_voices = 0, right_voices = 0;
		int diff_l, diff_r;

		if (tick_left == 0) {
			size_t before = q.count;

			tick_left = zz9k_pt4_events_tick(&ev, push, &q);
			if (tick_left == 0) {
				ended = 1;
				break;
			}
			for (; before < q.count; before++)
				if (q.cmd[q.head + before].op ==
				    AUDIO_VOICE_OP_START)
					starts++;
		}
		if (pt4_render_s16(ref, ref_out, 1, ZZ9K_PT4_RATE) != 1) {
			CHECK(0, "%s: reference ended at frame %u while "
			      "events still run", name, frame);
			break;
		}
		used = audio_voice_render(&e, out, 1, q.cmd + q.head, q.count);
		q.head += used;
		q.count -= used;
		tick_left--;

		for (k = 0; k < zz9k_pt4_events_voices(&ev); k++) {
			const pt4_voice *rv = &ref->st.v[k];
			uint32_t pos, frac;
			int active = audio_voice_position(&e, (uint32_t)k,
			                                  &pos, &frac);

			if (active != rv->active ||
			    (active && (pos != rv->pos || frac != rv->frac))) {
				if (pos_mismatch++ < 5)
					printf("%s: frame %u voice %d: engine "
					       "%d %u.%u, libpt4 %d %u.%u\n",
					       name, frame, k, active, pos,
					       frac, rv->active, rv->pos,
					       rv->frac);
			}
			if (rv->active) {
				if (on_left(k))
					left_voices++;
				else
					right_voices++;
			}
		}
		diff_l = abs((int)out[0] - (int)ref_out[0]);
		diff_r = abs((int)out[1] - (int)ref_out[1]);
		if ((uint32_t)diff_l > max_diff)
			max_diff = (uint32_t)diff_l;
		if ((uint32_t)diff_r > max_diff)
			max_diff = (uint32_t)diff_r;
		if (diff_l > left_voices + 1 || diff_r > right_voices + 1) {
			if (pcm_mismatch++ < 5)
				printf("%s: frame %u: engine %d/%d, libpt4 "
				       "%d/%d\n", name, frame, out[0], out[1],
				       ref_out[0], ref_out[1]);
		}
	}

	CHECK(!q.overflow, "%s: command queue overflow", name);
	CHECK(starts > 0, "%s: no note was started", name);
	CHECK(e.late == 0, "%s: %u late commands", name, e.late);
	CHECK(e.rejected == 0, "%s: %u rejected commands", name, e.rejected);
	CHECK(pos_mismatch == 0, "%s: %u voice position mismatches", name,
	      pos_mismatch);
	CHECK(pcm_mismatch == 0, "%s: %u output frames beyond rounding", name,
	      pcm_mismatch);
	printf("%s: %u frames, %u notes started, max output difference %u\n",
	       name, frame, starts, max_diff);
out:
	pt4_close(ref);
	pt4_close(drv);
	free(data);
}

/* EFx (funk repeat) rewrites sample bytes during play, so a module using it
 * must be refused on any channel, including the wide channels libpt4's own
 * load-time scan does not look at. */
static void funk_module_refused(int channel)
{
	uint8_t *data;
	size_t len = 0, cell;
	pt4_song *s;
	ZZ9KPt4Events ev;

	data = load_file("fixtures/pt4/32ch.mod", &len);
	CHECK(data != NULL, "cannot read 32ch.mod");
	if (data == NULL)
		return;
	/* Pattern 0, row 1: 1084-byte header, 32 cells of 4 bytes per row. */
	cell = 1084U + (1U * 32U + (size_t)channel) * 4U;
	data[cell + 2] = (uint8_t)((data[cell + 2] & 0xf0) | 0x0e);
	data[cell + 3] = 0xf1;
	s = open_song(data, len);
	if (s != NULL) {
		CHECK(zz9k_pt4_events_init(&ev, s, 0) == PT4_ERR_FORMAT,
		      "EF1 on channel %d must be refused", channel + 1);
		pt4_close(s);
	}
	free(data);
}

static void dyn_module_refused(void)
{
	uint8_t *data;
	size_t len = 0;
	pt4_song *s;
	ZZ9KPt4Events ev;

	data = load_file("fixtures/pt4/dyn_loop.mod", &len);
	CHECK(data != NULL, "cannot read dyn_loop.mod");
	if (data == NULL)
		return;
	s = open_song(data, len);
	if (s != NULL) {
		CHECK(zz9k_pt4_events_init(&ev, s, 0) == PT4_ERR_FORMAT,
		      "a module with DYN envelopes must be refused");
		pt4_close(s);
	}
	free(data);
}

int main(void)
{
	/* Wide modules: 32 and 9 channels. */
	compare_module("32ch");
	compare_module("9chn");
	/* 8-channel modules on the same path: backward play (81y), loops
	 * and sample offsets beyond 64 KB, tempo changes. */
	compare_module("fx81y");
	compare_module("loop64k8");
	compare_module("tempo8");
	/* A 4-channel song with 70 patterns on the same path: arpeggio,
	 * vibrato, portamento and volume slides over many notes. */
	compare_module("mkk_70pat");
	/* DYN envelopes play chunk lists, which voice events cannot carry. */
	dyn_module_refused();
	/* Funk repeat cannot be followed by an uploaded bank, on any channel. */
	funk_module_refused(0);
	funk_module_refused(4);
	funk_module_refused(31);

	if (failures) {
		printf("audio_voice_pt4_test: %d failure(s)\n", failures);
		return 1;
	}
	printf("audio_voice_pt4_test: all modules match libpt4\n");
	return 0;
}
