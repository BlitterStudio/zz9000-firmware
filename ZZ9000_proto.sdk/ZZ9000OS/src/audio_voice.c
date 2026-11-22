/*
 * ZZ9000AX voice engine core.
 *
 * Integer-only wavetable voice mixer. Position and loop rules follow
 * libpt4's WIDE path (libpt4/src/voice.c, advance() and the WIDE rate
 * branch of pt4__update_voices): position is an integer sample index plus
 * a 16-bit fraction, step = (hz << 16) / 48000, forward wrap lands at
 * loopstart + overshoot, backward wrap lands at end - under - 1.
 *
 * One deliberate difference: libpt4 addresses an arena where address 0 is
 * never played, so a backward step below address 0 leaves the voice parked
 * there. Sample indexes start at 0 here, so a step below index 0 counts as
 * passing below the loop start: a voice without a loop stops, a voice with
 * a loop wraps.
 *
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "audio_voice.h"

#include <string.h>

void audio_voice_init(struct audio_voice_engine *e, const uint8_t *bank,
                      uint32_t bank_len, uint32_t clock)
{
	memset(e, 0, sizeof(*e));
	e->bank = bank;
	e->bank_len = bank_len;
	e->clock = clock;
	e->interp = AUDIO_VOICE_INTERP_NEAREST;
	e->master = AUDIO_VOICE_MASTER_UNITY;
}

void audio_voice_silence(struct audio_voice_engine *e)
{
	uint32_t i;

	for (i = 0; i < AUDIO_VOICE_MAX_VOICES; i++)
		e->voice[i].active = 0;
}

int audio_voice_sample_set(struct audio_voice_engine *e, uint32_t id,
                           uint32_t offset, uint32_t length, uint32_t format)
{
	struct audio_voice_sample *s;

	if (id >= AUDIO_VOICE_MAX_SAMPLES)
		return -1;
	if (format != AUDIO_VOICE_FMT_S8 && format != AUDIO_VOICE_FMT_S16)
		return -1;
	if ((uint64_t)offset + length > e->bank_len)
		return -1;
	s = &e->sample[id];
	s->offset = offset;
	s->length = length;
	s->format = (uint8_t)format;
	s->defined = 1;
	return 0;
}

int audio_voice_position(const struct audio_voice_engine *e, uint32_t voice,
                         uint32_t *index, uint32_t *frac)
{
	if (voice >= AUDIO_VOICE_MAX_VOICES) {
		*index = 0;
		*frac = 0;
		return 0;
	}
	*index = e->voice[voice].pos;
	*frac = e->voice[voice].frac;
	return e->voice[voice].active;
}

static void set_rate(struct audio_voice_engine *e, struct audio_voice *v,
                     uint32_t flags, uint32_t rate)
{
	uint32_t hz;

	if (flags & AUDIO_VOICE_RATE_HZ) {
		hz = rate;
	} else {
		if (rate < AUDIO_VOICE_MIN_PERIOD)
			return;
		hz = e->clock / rate;
	}
	if (hz == 0U)
		return;
	if (hz > AUDIO_VOICE_MAX_HZ)
		hz = AUDIO_VOICE_MAX_HZ;
	v->step = (uint32_t)(((uint64_t)hz << 16) / AUDIO_VOICE_OUT_HZ);
}

static void start_voice(struct audio_voice_engine *e, struct audio_voice *v,
                        const struct audio_voice_cmd *c)
{
	const struct audio_voice_sample *s;
	uint32_t shift, nsamples, off, len, end;

	v->active = 0;
	if (c->flags >= AUDIO_VOICE_MAX_SAMPLES ||
	    !e->sample[c->flags].defined) {
		e->rejected++;
		return;
	}
	s = &e->sample[c->flags];
	shift = s->format == AUDIO_VOICE_FMT_S16 ? 1U : 0U;
	nsamples = s->length >> shift;
	off = c->arg0 >> shift;
	len = c->arg1 >> shift;
	if (off >= nsamples || (c->arg1 != 0U && len == 0U)) {
		e->rejected++;
		return;
	}
	end = nsamples;
	if (c->arg1 != 0U && (uint64_t)off + len < nsamples)
		end = off + len;

	v->s16 = (uint8_t)shift;
	v->base = s->offset;
	v->len = nsamples;
	v->pos = off;
	v->frac = 0;
	v->end = end;
	v->backward = 0;
	v->loopstart = 0;
	v->looplen = 0;
	v->active = 1;
}

static void set_loop(struct audio_voice_engine *e, struct audio_voice *v,
                     const struct audio_voice_cmd *c)
{
	uint32_t ls = c->arg0 >> v->s16;
	uint32_t ll = c->arg1 >> v->s16;

	if (ll == 0U) {
		if (c->arg1 == 0U && ls <= v->len) {
			v->loopstart = ls;
			v->looplen = 0;
			return;
		}
	} else if ((uint64_t)ls + ll <= v->len) {
		v->loopstart = ls;
		v->looplen = ll;
		return;
	}
	e->rejected++;
}

static void apply(struct audio_voice_engine *e,
                  const struct audio_voice_cmd *c)
{
	struct audio_voice *v;

	if (c->op == AUDIO_VOICE_OP_MASTER) {
		e->master = c->arg0 > AUDIO_VOICE_MASTER_MAX ?
		            AUDIO_VOICE_MASTER_MAX : c->arg0;
		return;
	}
	if (c->voice >= AUDIO_VOICE_MAX_VOICES) {
		e->rejected++;
		return;
	}
	v = &e->voice[c->voice];
	switch (c->op) {
	case AUDIO_VOICE_OP_START:
		start_voice(e, v, c);
		break;
	case AUDIO_VOICE_OP_LOOP:
		set_loop(e, v, c);
		break;
	case AUDIO_VOICE_OP_SET:
		if (c->flags & AUDIO_VOICE_SET_RATE)
			set_rate(e, v, c->flags, c->arg0);
		if (c->flags & AUDIO_VOICE_SET_VOLUME) {
			v->vol_l = c->arg1 >> 16;
			v->vol_r = c->arg1 & 0xFFFFU;
		}
		break;
	case AUDIO_VOICE_OP_STOP:
		v->active = 0;
		break;
	case AUDIO_VOICE_OP_DIRECTION:
		if (v->active)
			v->backward = (c->flags & AUDIO_VOICE_DIR_BACKWARD) ? 1U : 0U;
		break;
	default:
		e->rejected++;
		break;
	}
}

/* Sample at index i as 16-bit signed; 8-bit data is promoted by 8 bits. */
static int32_t sample_at(const struct audio_voice_engine *e,
                         const struct audio_voice *v, uint32_t i)
{
	if (v->s16) {
		const uint8_t *p = e->bank + v->base + 2U * i;

		return (int16_t)(uint16_t)(((uint32_t)p[0] << 8) | p[1]);
	}
	return (int32_t)(int8_t)e->bank[v->base + i] * 256;
}

static int32_t read_voice(const struct audio_voice_engine *e,
                          const struct audio_voice *v)
{
	int32_t a = sample_at(e, v, v->pos);

	if (e->interp == AUDIO_VOICE_INTERP_LINEAR) {
		uint32_t n;
		int32_t b;

		if (v->backward) {
			if (v->pos > v->loopstart)
				n = v->pos - 1U;
			else
				n = v->looplen != 0U ?
				    v->loopstart + v->looplen - 1U : v->pos;
		} else {
			if (v->pos + 1U < v->end)
				n = v->pos + 1U;
			else
				n = v->looplen != 0U ? v->loopstart : v->pos;
		}
		b = sample_at(e, v, n);
		a += (int32_t)(((int64_t)(b - a) * (int64_t)v->frac) >> 16);
	}
	return a;
}

static void advance(struct audio_voice *v)
{
	if (!v->backward) {
		uint32_t f = v->frac + (v->step & 0xFFFFU);

		v->pos += (v->step >> 16) + (f >> 16);
		v->frac = f & 0xFFFFU;
		if (v->pos < v->end)
			return;
		if (v->looplen == 0U) {
			v->active = 0;
		} else {
			uint32_t over = v->pos - v->end;

			if (over >= v->looplen)
				over = 0;
			v->pos = v->loopstart + over;
			v->end = v->loopstart + v->looplen;
		}
	} else {
		uint32_t sub = v->step & 0xFFFFU;
		uint32_t ip = v->step >> 16;
		int below;

		if (v->frac < sub) {
			v->frac = v->frac + 0x10000U - sub;
			ip++;
		} else {
			v->frac -= sub;
		}
		below = ip > v->pos;
		if (below) {
			v->pos = 0;
			v->frac = 0;
		} else {
			v->pos -= ip;
		}
		if (!below && v->pos >= v->loopstart)
			return;
		if (v->looplen == 0U) {
			v->active = 0;
		} else {
			uint32_t under = v->loopstart - v->pos;

			if (under >= v->looplen)
				under = 0;
			v->end = v->loopstart + v->looplen;
			v->pos = v->end - under - 1U;
		}
	}
}

static int16_t saturate(int64_t acc, uint32_t master)
{
	int64_t x = (acc * (int64_t)master) >> 8;

	if (x > 32767)
		return 32767;
	if (x < -32768)
		return -32768;
	return (int16_t)x;
}

static void mix(struct audio_voice_engine *e, int16_t *out, uint32_t frames)
{
	uint32_t i, k;

	for (i = 0; i < frames; i++) {
		int32_t acc_l = 0;
		int32_t acc_r = 0;

		for (k = 0; k < AUDIO_VOICE_MAX_VOICES; k++) {
			struct audio_voice *v = &e->voice[k];
			int32_t s;

			if (!v->active || v->step == 0U)
				continue;
			s = read_voice(e, v);
			acc_l += (s * (int32_t)v->vol_l) >> 16;
			acc_r += (s * (int32_t)v->vol_r) >> 16;
			advance(v);
		}
		out[2U * i] = saturate(acc_l, e->master);
		out[2U * i + 1U] = saturate(acc_r, e->master);
	}
}

size_t audio_voice_render(struct audio_voice_engine *e, int16_t *out,
                          uint32_t frames, const struct audio_voice_cmd *cmds,
                          size_t ncmds)
{
	uint32_t at = 0;
	size_t used = 0;

	while (used < ncmds) {
		int32_t rel = (int32_t)(cmds[used].stamp - e->frame);
		int late = rel < 0;

		if (late)
			rel = 0;
		if ((uint32_t)rel >= frames)
			break;
		e->late += (uint32_t)late;
		if ((uint32_t)rel > at) {
			mix(e, out + 2U * at, (uint32_t)rel - at);
			at = (uint32_t)rel;
		}
		apply(e, &cmds[used]);
		used++;
	}
	if (at < frames)
		mix(e, out + 2U * at, frames - at);
	e->frame += frames;
	return used;
}
