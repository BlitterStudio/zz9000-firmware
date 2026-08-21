/*
 * Firmware-authoritative audio control plane arbiter (plan U2, KTD2):
 * scene state, split authority, gain staging, and -- since U3 -- the
 * per-direction metering accumulators with coherent snapshots (KTD3).
 *
 * Holds the scene state, the operator baseline balance, per-owner
 * source trims, the enforced saturation boundary, and the
 * gain-reduction telemetry accumulator. Every master-chain write
 * reaches the ADAU1701 through this module's calls into the ax.h
 * setters -- the legacy register path is gated by
 * audio_scene_register_write_blocked() in main.c (R2).
 *
 * Metering (U3, KTD3): per-direction single-writer volatile
 * accumulators fed by the audio formatter ISRs through bounded
 * per-period scans, and seqlock-framed coherent snapshots consumed by
 * the meter-read dispatch. See the metering section below.
 *
 * Gain staging (R7) composes conservatively: the summed applied mixer
 * legs (operator baseline plus owner trims) multiplied by the scene's
 * prefactor, applied output volume, and worst-case positive EQ band
 * gain, evaluated against AUDIO_SCENE_ENFORCED_BOUNDARY. A composition
 * over the boundary is reduced -- the mixer sum first (reported back
 * to the requesting owner), the applied output volume when the scene
 * alone exceeds it -- and each reduction emits exactly one
 * gain-reduction event.
 *
 * Host tests link this file without ax.c and provide their own
 * definitions of the five DSP setters (see test/audio/
 * audio_scene_test.c).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>

#include "audio_dsp_gain.h"
#include "audio_scene.h"
#include "sdk_mailbox.h"
#include "ax.h"

/* Operator baseline default: today's power-on mixer state written by
 * audio_adau_init (Paula 128, AX 64; 127 = 0 dB). */
#define BASELINE_DEFAULT_PAULA 128
#define BASELINE_DEFAULT_AX 64

/* LPF cutoff range accepted in scene definitions (23900 Hz is the
 * DSP default; above it approaches the 48 kHz Nyquist). */
#define LPF_HZ_MIN 1
#define LPF_HZ_MAX 23900

/*
 * Fixed scene slots (label = slot number). Every default composes to
 * at or below the enforced boundary with the default baseline summed
 * (192): the worst defaults apply 192 * prefactor * volume * EQ-boost
 * <= 192. Slot 0 is today's power-on default (LPF 23900, unity EQ,
 * unity prefactor, full volume) and composes exactly at the boundary.
 */
static const struct audio_scene_def default_scenes[AUDIO_SCENE_COUNT] = {
	{ 23900, { 50, 50, 50, 50, 50, 50, 50, 50, 50, 50 }, 50, 100, 50 },
	{ 23900, { 50, 50, 50, 50, 50, 50, 50, 50, 50, 50 }, 50, 80, 50 },
	{ 16000, { 55, 55, 50, 50, 50, 50, 50, 50, 50, 50 }, 50, 75, 50 },
	{ 18000, { 50, 50, 50, 50, 50, 50, 55, 55, 50, 50 }, 50, 75, 50 },
	{ 23900, { 50, 50, 50, 50, 50, 50, 50, 50, 50, 50 }, 60, 70, 50 },
	{ 12000, { 45, 45, 50, 50, 50, 50, 50, 50, 45, 45 }, 50, 90, 50 },
	{ 23900, { 50, 50, 50, 50, 50, 50, 50, 50, 50, 50 }, 55, 75, 50 },
	{ 23900, { 50, 50, 50, 50, 50, 50, 50, 50, 50, 50 }, 50, 60, 50 },
};

static struct audio_scene_def scenes[AUDIO_SCENE_COUNT];
static uint8_t active_scene_index;
static uint8_t baseline_paula = BASELINE_DEFAULT_PAULA;
static uint8_t baseline_ax = BASELINE_DEFAULT_AX;

struct trim_state {
	int16_t paula;
	int16_t ax;
};

static struct trim_state trims[AUDIO_SCENE_OWNER_SLOTS];
static uint8_t owner_participating[AUDIO_SCENE_OWNER_SLOTS];

static int module_initialized;
static int authority_claimed;
static uint32_t gain_reduction_count;
static struct audio_scene_gain_event last_gain_reduction;
static int have_last_gain_reduction;

/* ---- gain arithmetic ---- */

/* Worst-case positive boost across the EQ bands; never below unity,
 * so cuts never reduce the composed level in staging (R7). */
static double eq_worst_boost_linear(const struct audio_scene_def *scene)
{
	double worst = 1.0;
	int i;

	for (i = 0; i < AUDIO_SCENE_EQ_BANDS; i++) {
		if (scene->eq[i] > 50) {
			double db = ((double)scene->eq[i] - 50.0) * 12.0 /
				50.0;
			double linear = pow(10.0, db / 20.0);
			if (linear > worst)
				worst = linear;
		}
	}
	return worst;
}

static double master_chain_linear(const struct audio_scene_def *scene,
	double volume_linear)
{
	return audio_adau_prefactor_gain(scene->prefactor) * volume_linear *
		eq_worst_boost_linear(scene);
}

static double baseline_sum_linear(void)
{
	return (double)baseline_paula + (double)baseline_ax;
}

struct volume_resolution {
	int applied_volume; /* written to audio_adau_set_vol_pan */
	double linear;      /* applied_volume / 100 */
	int reduced;        /* scene-alone level exceeded the boundary */
	double requested_level;
	double applied_level;
};

/*
 * Scene-alone evaluation (R7): the scene's own master-chain level --
 * neutral baseline, no trims, no requester. When it exceeds the
 * enforced boundary, the applied output volume is reduced to fit and
 * the stored scene definition is left untouched (the operator sees it
 * through telemetry; save-time validation is the persistence unit's).
 */
static void resolve_output_volume(const struct audio_scene_def *scene,
	struct volume_resolution *out)
{
	double volume_linear = ((double)scene->volume) / 100.0;
	double chain = master_chain_linear(scene, 1.0);
	double level = baseline_sum_linear() * chain * volume_linear;

	out->applied_volume = scene->volume;
	out->linear = volume_linear;
	out->reduced = 0;
	out->requested_level = level;
	out->applied_level = level;
	if (level > AUDIO_SCENE_ENFORCED_BOUNDARY) {
		double allowed = AUDIO_SCENE_ENFORCED_BOUNDARY /
			(baseline_sum_linear() * chain);
		if (allowed > 1.0)
			allowed = 1.0;
		out->applied_volume = (int)(allowed * 100.0);
		out->linear = ((double)out->applied_volume) / 100.0;
		out->reduced = 1;
		out->applied_level = baseline_sum_linear() * chain *
			out->linear;
	}
}

struct mixer_stage {
	uint8_t bounded;
	uint8_t paula; /* applied mixer legs (absolute) */
	uint8_t ax;
	double trim_bound;  /* summed-trim headroom for this scene+baseline */
	double requested;   /* composed level when bounded */
	double applied;     /* composed level when bounded */
};

static int clamp_u8(int value)
{
	if (value < 0)
		return 0;
	if (value > 255)
		return 255;
	return value;
}

/*
 * Baseline + trim composition against the boundary: the summed mixer
 * legs are proportionally reduced so the composed level fits. Pure
 * computation; the caller emits the event and writes the legs.
 */
static void compute_mixer_stage(double master_linear,
	struct mixer_stage *out)
{
	double max_sum;
	int paula = baseline_paula;
	int ax = baseline_ax;
	int owner;

	for (owner = AUDIO_SCENE_OWNER_AHI;
			owner < AUDIO_SCENE_OWNER_SLOTS; owner++) {
		paula += trims[owner].paula;
		ax += trims[owner].ax;
	}
	paula = clamp_u8(paula);
	ax = clamp_u8(ax);

	memset(out, 0, sizeof(*out));
	out->paula = (uint8_t)paula;
	out->ax = (uint8_t)ax;

	if (master_linear <= 0.0)
		return; /* chain is silent: nothing can exceed */

	max_sum = AUDIO_SCENE_ENFORCED_BOUNDARY / master_linear;
	out->trim_bound = max_sum - baseline_sum_linear();
	if (out->trim_bound < 0.0)
		out->trim_bound = 0.0;

	if ((double)(paula + ax) > max_sum) {
		double scale = max_sum / (double)(paula + ax);
		int c_paula = (int)((double)paula * scale);
		int c_ax = (int)((double)ax * scale);

		out->bounded = 1;
		out->paula = (uint8_t)clamp_u8(c_paula);
		out->ax = (uint8_t)clamp_u8(c_ax);
		out->requested = (double)(paula + ax) * master_linear;
		out->applied = (double)(c_paula + c_ax) * master_linear;
	}
}

static void emit_gain_reduction(double requested, double applied)
{
	last_gain_reduction.requested = requested;
	last_gain_reduction.applied = applied;
	last_gain_reduction.boundary = AUDIO_SCENE_ENFORCED_BOUNDARY;
	have_last_gain_reduction = 1;
	if (gain_reduction_count != UINT32_MAX)
		gain_reduction_count++;
	printf("[scene] gain reduction: %.1f -> %.1f (boundary %.1f)\n",
		requested, applied, AUDIO_SCENE_ENFORCED_BOUNDARY);
}

/* Re-stage only the mixer (trim submit/release): the applied master
 * chain stays as the last apply resolved it. */
static int restage_mixer(struct mixer_stage *stage)
{
	struct volume_resolution vol;

	resolve_output_volume(&scenes[active_scene_index], &vol);
	compute_mixer_stage(
		master_chain_linear(&scenes[active_scene_index], vol.linear),
		stage);
	if (stage->bounded)
		emit_gain_reduction(stage->requested, stage->applied);
	return audio_adau_set_mixer_vol(stage->paula, stage->ax);
}

/*
 * The single apply path (select, edit commit, baseline change, DSP
 * re-init): ordered verified writes -- LPF, EQ bands 0..9, prefactor,
 * output volume/pan, staged mixer. The fade-commit-restore wrapper
 * for live switching joins this in the dispatch unit (KTD7).
 */
static int apply_active_scene(void)
{
	const struct audio_scene_def *scene = &scenes[active_scene_index];
	struct volume_resolution vol;
	struct mixer_stage stage;
	int i;

	resolve_output_volume(scene, &vol);
	if (vol.reduced)
		emit_gain_reduction(vol.requested_level, vol.applied_level);

	if (audio_adau_set_lpf_params(scene->lpf_hz) != 0)
		return -1;
	for (i = 0; i < AUDIO_SCENE_EQ_BANDS; i++) {
		if (audio_adau_set_eq_gain(i, scene->eq[i]) != 0)
			return -1;
	}
	if (audio_adau_set_prefactor(scene->prefactor) != 0)
		return -1;
	if (audio_adau_set_vol_pan(vol.applied_volume, scene->pan) != 0)
		return -1;

	compute_mixer_stage(master_chain_linear(scene, vol.linear), &stage);
	if (stage.bounded)
		emit_gain_reduction(stage.requested, stage.applied);
	if (audio_adau_set_mixer_vol(stage.paula, stage.ax) != 0)
		return -1;
	return 0;
}

/* ---- metering accumulation and coherent snapshots (U3, KTD3) ---- */

enum { METER_OUTPUT = 0, METER_CAPTURE = 1, METER_DIRECTIONS = 2 };

struct audio_meter_dir {
	/* Seqlock generation (R9): the direction's single ISR writer
	 * bumps it around every update; the main-loop reader retries
	 * while it changes. Writers and reader share core 0 (the audio
	 * formatter IRQs never run concurrently with the mailbox
	 * dispatch), so compiler barriers are sufficient. */
	volatile uint32_t generation;
	volatile uint32_t peak_hold_ch1; /* unsigned 16.16 (R8) */
	volatile uint32_t peak_hold_ch2;
	volatile uint32_t clip_count;    /* at-rail regions; saturates */
	volatile uint32_t underrun_count;
	volatile uint32_t overrun_count;
	volatile uint32_t read_seq;      /* consume-on-read window mark */
	/* Writer-private (ISR context only). */
	uint32_t seen_read_seq;
	uint32_t clip_open;
	/* Main-loop context (pump bind/unbind) plus the pump ISR's
	 * abrupt-unbind path: plain aligned stores, read only by the
	 * snapshot. */
	uint32_t identity;
};

static struct audio_meter_dir meters[METER_DIRECTIONS];

/* Producer-stagnation state for register-fed playback: arm() writes
 * come from the main loop (playback enable), tick() from the TX ISR. */
static volatile uint32_t producer_armed;
static volatile uint32_t producer_arm_seq;
static volatile uint32_t producer_last_seq;

static void meter_writer_begin(struct audio_meter_dir *m)
{
	m->generation++;
	__asm__ __volatile__("" ::: "memory");
}

static void meter_writer_end(struct audio_meter_dir *m)
{
	__asm__ __volatile__("" ::: "memory");
	m->generation++;
}

/* Saturating bump (sdk_media_session.c discipline): never wraps. */
static void meter_saturating_add(volatile uint32_t *counter, uint32_t n)
{
	if (*counter > UINT32_MAX - n)
		*counter = UINT32_MAX;
	else
		*counter += n;
}

static int meter_at_rail(int32_t sample)
{
	return sample == INT16_MAX || sample == INT16_MIN;
}

static void meter_fold_frame(int32_t left, int32_t right,
	uint32_t *peak1, uint32_t *peak2, uint32_t *regions, int *open)
{
	uint32_t al = left < 0 ? (uint32_t)(-left) : (uint32_t)left;
	uint32_t ar = right < 0 ? (uint32_t)(-right) : (uint32_t)right;

	if (al > *peak1)
		*peak1 = al;
	if (ar > *peak2)
		*peak2 = ar;
	if (meter_at_rail(left) || meter_at_rail(right)) {
		if (!*open) {
			*open = 1;
			(*regions)++;
		}
	} else {
		*open = 0;
	}
}

static void meter_feed_period(struct audio_meter_dir *m,
	uint32_t peak1, uint32_t peak2, uint32_t regions, int clip_open)
{
	meter_writer_begin(m);
	if (m->seen_read_seq != m->read_seq) {
		/* A read marked the window boundary (R8): this period
		 * opens a fresh peak-hold window. */
		m->seen_read_seq = m->read_seq;
		m->peak_hold_ch1 = peak1 << 1;
		m->peak_hold_ch2 = peak2 << 1;
	} else {
		if ((peak1 << 1) > m->peak_hold_ch1)
			m->peak_hold_ch1 = peak1 << 1;
		if ((peak2 << 1) > m->peak_hold_ch2)
			m->peak_hold_ch2 = peak2 << 1;
	}
	m->clip_open = clip_open ? 1U : 0U;
	meter_saturating_add(&m->clip_count, regions);
	meter_writer_end(m);
}

void audio_scene_meter_output_period(const int16_t *frames,
	uint32_t frame_count)
{
	struct audio_meter_dir *m = &meters[METER_OUTPUT];
	uint32_t peak1 = 0, peak2 = 0, regions = 0;
	uint32_t i;
	int open = m->clip_open != 0;

	for (i = 0; i < frame_count; i++) {
		meter_fold_frame(frames[i * 2U], frames[i * 2U + 1U],
			&peak1, &peak2, &regions, &open);
	}
	meter_feed_period(m, peak1, peak2, regions, open);
}

void audio_scene_meter_capture_period(const uint8_t *period_be,
	uint32_t frame_count)
{
	struct audio_meter_dir *m = &meters[METER_CAPTURE];
	uint32_t peak1 = 0, peak2 = 0, regions = 0;
	uint32_t i;
	int open = m->clip_open != 0;

	for (i = 0; i < frame_count; i++) {
		const uint8_t *frame = &period_be[i * 4U];
		int32_t left = (int16_t)(((uint16_t)frame[0] << 8) |
		                         (uint16_t)frame[1]);
		int32_t right = (int16_t)(((uint16_t)frame[2] << 8) |
		                          (uint16_t)frame[3]);

		meter_fold_frame(left, right, &peak1, &peak2, &regions,
			&open);
	}
	meter_feed_period(m, peak1, peak2, regions, open);
}

void audio_scene_meter_output_underrun(void)
{
	struct audio_meter_dir *m = &meters[METER_OUTPUT];

	meter_writer_begin(m);
	meter_saturating_add(&m->underrun_count, 1U);
	meter_writer_end(m);
}

void audio_scene_meter_capture_overrun(void)
{
	struct audio_meter_dir *m = &meters[METER_CAPTURE];

	meter_writer_begin(m);
	meter_saturating_add(&m->overrun_count, 1U);
	meter_writer_end(m);
}

void audio_scene_meter_output_producer_arm(uint32_t producer_seq)
{
	producer_armed = 0;
	producer_arm_seq = producer_seq;
	producer_last_seq = producer_seq;
}

void audio_scene_meter_output_producer_tick(uint32_t producer_seq)
{
	if (!producer_armed) {
		/* The pre-filled startup ring is not an underrun: wait
		 * for the producer's first refill after playback was
		 * enabled. */
		if (producer_seq != producer_arm_seq) {
			producer_armed = 1;
			producer_last_seq = producer_seq;
		}
		return;
	}
	if (producer_seq == producer_last_seq)
		audio_scene_meter_output_underrun();
	producer_last_seq = producer_seq;
}

void audio_scene_meter_output_identity(uint32_t identity)
{
	meters[METER_OUTPUT].identity = identity;
}

static uint32_t meter_output_identity(void)
{
	uint32_t identity = meters[METER_OUTPUT].identity;

	/* A participating AHI owner (it holds a submitted source trim)
	 * is named while it drives the output; a control-plane session
	 * binding outranks it, and register-fed legacy playback stays
	 * legacy/unknown (R8). */
	if (identity == SDK_AUDIO_METER_IDENTITY_UNKNOWN &&
	    owner_participating[AUDIO_SCENE_OWNER_AHI])
		return SDK_AUDIO_METER_IDENTITY_AHI;
	return identity;
}

int audio_scene_meter_read(uint32_t direction, uint32_t flags,
	struct audio_meter_snapshot *snapshot)
{
	struct audio_meter_dir *m;

	if (snapshot == NULL)
		return -1;
	if (direction == SDK_AUDIO_METER_DIRECTION_OUTPUT)
		m = &meters[METER_OUTPUT];
	else if (direction == SDK_AUDIO_METER_DIRECTION_CAPTURE)
		m = &meters[METER_CAPTURE];
	else
		return -1;

	for (;;) {
		uint32_t g1 = m->generation;

		if ((g1 & 1U) == 0U) {
			snapshot->direction = direction;
			snapshot->generation = g1;
			snapshot->peak_hold_ch1 = m->peak_hold_ch1;
			snapshot->peak_hold_ch2 = m->peak_hold_ch2;
			snapshot->clip_count = m->clip_count;
			snapshot->underrun_count = m->underrun_count;
			snapshot->overrun_count = m->overrun_count;
			__asm__ __volatile__("" ::: "memory");
			if (m->generation == g1)
				break;
		}
	}
	snapshot->identity =
		(direction == SDK_AUDIO_METER_DIRECTION_OUTPUT)
		? meter_output_identity()
		: SDK_AUDIO_METER_IDENTITY_UNKNOWN;
	snapshot->gain_reduction_events =
		(direction == SDK_AUDIO_METER_DIRECTION_OUTPUT)
		? gain_reduction_count : 0U;
	if ((flags & SDK_AUDIO_METER_RESULT_HOLD_RESET) != 0U) {
		/* Consume-on-read (R8): the ISR writer restarts the
		 * peak-hold window at the next completed period -- the
		 * read itself stays a pure observer of the hold. */
		m->read_seq++;
	}
	return 0;
}

/* Reset the measured instance. Called from boot/warm-reset context
 * with playback torn down: the ISRs are not feeding meters there. */
static void meter_reset_all(void)
{
	int d;

	for (d = 0; d < METER_DIRECTIONS; d++) {
		struct audio_meter_dir *m = &meters[d];

		m->generation = 0;
		m->peak_hold_ch1 = 0;
		m->peak_hold_ch2 = 0;
		m->clip_count = 0;
		m->underrun_count = 0;
		m->overrun_count = 0;
		m->read_seq = 0;
		m->seen_read_seq = 0;
		m->clip_open = 0;
		m->identity = SDK_AUDIO_METER_IDENTITY_UNKNOWN;
	}
	producer_armed = 0;
	producer_arm_seq = 0;
	producer_last_seq = 0;
}

/* ---- public surface ---- */

void audio_scene_init(void)
{
	memcpy(scenes, default_scenes, sizeof(scenes));
	active_scene_index = 0;
	baseline_paula = BASELINE_DEFAULT_PAULA;
	baseline_ax = BASELINE_DEFAULT_AX;
	memset(trims, 0, sizeof(trims));
	memset(owner_participating, 0, sizeof(owner_participating));
	gain_reduction_count = 0;
	have_last_gain_reduction = 0;
	meter_reset_all();
	module_initialized = 1;
	authority_claimed = 1;
}

int audio_scene_apply_after_dsp_init(void)
{
	if (!module_initialized)
		return -1;
	/* The DSP instance the counters described was just replaced, and
	 * the reset tore every owner session down (R10). */
	gain_reduction_count = 0;
	have_last_gain_reduction = 0;
	memset(trims, 0, sizeof(trims));
	memset(owner_participating, 0, sizeof(owner_participating));
	meter_reset_all();
	return apply_active_scene();
}

static int scene_def_valid(const struct audio_scene_def *def)
{
	int i;

	if (def->lpf_hz < LPF_HZ_MIN || def->lpf_hz > LPF_HZ_MAX)
		return 0;
	if (def->prefactor > 100 || def->volume > 100 || def->pan > 100)
		return 0;
	for (i = 0; i < AUDIO_SCENE_EQ_BANDS; i++)
		if (def->eq[i] > 100)
			return 0;
	return 1;
}

int audio_scene_select(uint8_t index)
{
	if (index >= AUDIO_SCENE_COUNT || !module_initialized)
		return -1;
	active_scene_index = index;
	return apply_active_scene();
}

int audio_scene_write(uint8_t index, const struct audio_scene_def *def)
{
	if (index >= AUDIO_SCENE_COUNT || def == NULL ||
			!module_initialized)
		return -1;
	if (!scene_def_valid(def))
		return -1;
	scenes[index] = *def;
	if (index == active_scene_index)
		return apply_active_scene();
	return 0;
}

const struct audio_scene_def *audio_scene_get(uint8_t index)
{
	if (index >= AUDIO_SCENE_COUNT)
		return NULL;
	return &scenes[index];
}

uint8_t audio_scene_active_index(void)
{
	return active_scene_index;
}

void audio_scene_set_baseline(uint8_t paula, uint8_t ax)
{
	if (!module_initialized)
		return;
	baseline_paula = paula;
	baseline_ax = ax;
	/* The baseline participates in the scene-alone level, so the
	 * change re-runs the one apply path. */
	apply_active_scene();
}

uint8_t audio_scene_baseline_paula(void)
{
	return baseline_paula;
}

uint8_t audio_scene_baseline_ax(void)
{
	return baseline_ax;
}

int audio_scene_trim_submit(uint8_t owner, int16_t paula, int16_t ax,
	struct audio_scene_trim_result *result)
{
	struct mixer_stage stage;

	if (result != NULL)
		memset(result, 0, sizeof(*result));
	if (owner < AUDIO_SCENE_OWNER_AHI || owner >=
			AUDIO_SCENE_OWNER_SLOTS || !module_initialized)
		return -1;

	trims[owner].paula = paula;
	trims[owner].ax = ax;
	owner_participating[owner] = 1;
	if (restage_mixer(&stage) != 0)
		return -1;

	if (result != NULL) {
		result->bounded = stage.bounded;
		result->mixer_paula = stage.paula;
		result->mixer_ax = stage.ax;
		result->trim_bound = stage.trim_bound;
	}
	return 0;
}

void audio_scene_trim_release(uint8_t owner)
{
	struct mixer_stage stage;

	if (owner < AUDIO_SCENE_OWNER_AHI ||
			owner >= AUDIO_SCENE_OWNER_SLOTS ||
			!module_initialized)
		return;
	trims[owner].paula = 0;
	trims[owner].ax = 0;
	owner_participating[owner] = 0;
	restage_mixer(&stage);
}

int audio_scene_authority_active(void)
{
	return authority_claimed;
}

int audio_scene_register_write_blocked(uint32_t audio_param)
{
	/* Master-chain parameters (ax.h AP_DSP_SET_LOWPASS ..
	 * AP_DSP_SET_STEREO_VOLUME) are never writable through the
	 * register path: a register write is by definition not issued
	 * by the scene module. The raw DSP upload is closed behind the
	 * same authority once the control plane is running (R2). */
	if (audio_param >= AP_DSP_SET_LOWPASS &&
			audio_param <= AP_DSP_SET_STEREO_VOLUME)
		return 1;
	if (audio_param == AP_DSP_UPLOAD)
		return authority_claimed;
	return 0;
}

uint32_t audio_scene_gain_reduction_events(void)
{
	return gain_reduction_count;
}

const struct audio_scene_gain_event *
audio_scene_last_gain_reduction(void)
{
	if (!have_last_gain_reduction)
		return NULL;
	return &last_gain_reduction;
}
