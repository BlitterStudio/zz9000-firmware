/*
 * Shared recording-stub infrastructure for the audio control-plane
 * host tests: a write log the five ax.h DSP setter stubs feed, plus
 * query helpers. Each test defines its own stub bodies (failure
 * injection, nested-commit reentry) on top of this log.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef DSP_WRITE_MOCK_H
#define DSP_WRITE_MOCK_H

enum {
	WRITE_LPF,   /* audio_adau_set_lpf_params(f0) */
	WRITE_MIXER, /* audio_adau_set_mixer_vol(vol1, vol2) */
	WRITE_PREF,  /* audio_adau_set_prefactor(pre) */
	WRITE_EQ,    /* audio_adau_set_eq_gain(band, gain) */
	WRITE_VOLPAN /* audio_adau_set_vol_pan(vol, pan) */
};

#define WRITE_LOG_MAX 512

struct dsp_write {
	int kind;
	int a;
	int b;
};

static struct dsp_write write_log[WRITE_LOG_MAX];
static int write_count;
static int fail_next_write; /* injects a verified-write failure */

static void record_write(int kind, int a, int b)
{
	if (write_count < WRITE_LOG_MAX) {
		write_log[write_count].kind = kind;
		write_log[write_count].a = a;
		write_log[write_count].b = b;
	}
	write_count++;
}

static void clear_writes(void)
{
	write_count = 0;
	fail_next_write = 0;
}

__attribute__((unused))
static int log_at(int index, int *kind, int *a, int *b)
{
	if (index < 0 || index >= write_count || index >= WRITE_LOG_MAX)
		return 0;
	*kind = write_log[index].kind;
	*a = write_log[index].a;
	*b = write_log[index].b;
	return 1;
}

/* Last recorded write of a kind; returns 0 when none exists. */
__attribute__((unused))
static int last_write(int kind, int *a, int *b)
{
	int i;
	for (i = write_count - 1; i >= 0 && i < WRITE_LOG_MAX; i--) {
		if (write_log[i].kind == kind) {
			*a = write_log[i].a;
			*b = write_log[i].b;
			return 1;
		}
	}
	return 0;
}

__attribute__((unused))
static int count_writes(int kind)
{
	int i;
	int n = 0;
	for (i = 0; i < write_count && i < WRITE_LOG_MAX; i++)
		if (write_log[i].kind == kind)
			n++;
	return n;
}

#endif /* DSP_WRITE_MOCK_H */
