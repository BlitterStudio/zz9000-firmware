/*
 * Host tests for the ZZ9000AX audio control plane arbiter
 * (audio_scene.c): authority gate over the legacy register path, gain
 * staging against the enforced saturation boundary, boot/warm-reset
 * apply order, and the trim lifecycle (plan U2).
 *
 * audio_scene.c is linked without ax.c: this harness provides the
 * ax.h DSP setters as recording stubs (link-time seam), so every
 * master-chain write the arbiter issues is observable, in order --
 * the dispatcher-coverage contract that nothing bypasses the arbiter.
 */

#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_scene.h"
#include "ax.h"
#include "sdk_mailbox.h"

/* ---- recording stubs for the ax.h DSP setters ---- */

#include "dsp_write_mock.h"

/* ---- commit-machine drain (the firmware main loop polls) ---- */

/* audio_scene.c runs its commit as an incremental machine: the
 * dispatch entry points start it and return before any I2C, and the
 * main loop advances it one setter call per pass. Tests drain it
 * synchronously to its terminal state before asserting on the write
 * log. */
static void pump_scene(void)
{
	int guard = 0;

	while (audio_scene_poll() && ++guard < 1000)
		;
}



int audio_adau_set_lpf_params(int f0)
{
	record_write(WRITE_LPF, f0, 0);
	return fail_next_write ? -1 : 0;
}

int audio_adau_set_mixer_vol(int vol1, int vol2)
{
	record_write(WRITE_MIXER, vol1, vol2);
	return fail_next_write ? -1 : 0;
}

int audio_adau_set_prefactor(int pre)
{
	record_write(WRITE_PREF, pre, 0);
	return fail_next_write ? -1 : 0;
}

/* Mid-sequence failure seam: fail the EQ write for this band (the
 * commit-failure tests), on top of the shared first-write seam. */
static int fail_eq_band = -1;

int audio_adau_set_eq_gain(int band, int gain)
{
	record_write(WRITE_EQ, band, gain);
	if (band == fail_eq_band) {
		fail_eq_band = -1;
		return -1;
	}
	return fail_next_write ? -1 : 0;
}

/* Restore-write failure seam: fails the vol_pan writes that bring
 * the output back up (vol != 0) while letting the fade through. */
static int fail_volpan_restore;

int audio_adau_set_vol_pan(int vol, int pan)
{
	record_write(WRITE_VOLPAN, vol, pan);
	if (fail_volpan_restore && vol != 0)
		return -1;
	return fail_next_write ? -1 : 0;
}

/* ---- assertions (audio_convert_test convention) ---- */

static int failures;

static void check(int ok, const char *name, const char *detail)
{
	if (!ok) {
		failures++;
		printf("FAILED: %s (%s)\n", name, detail ? detail : "");
	}
}

static const char *fmt(const char *format, ...)
{
	static char buffer[160];
	va_list args;
	va_start(args, format);
	vsnprintf(buffer, sizeof(buffer), format, args);
	va_end(args);
	return buffer;
}

static int near(double actual, double expected)
{
	double scale = expected < 1.0 ? 1.0 : expected;
	return fabs(actual - expected) <= 1.0e-6 * scale;
}

#define DB12_TO_LIN(db) pow(10.0, (db) / 20.0)

static void unity_scene(struct audio_scene_def *def)
{
	int i;
	def->lpf_hz = 23900;
	for (i = 0; i < AUDIO_SCENE_EQ_BANDS; i++)
		def->eq[i] = 50;
	def->prefactor = 50;
	def->volume = 100;
	def->pan = 50;
}

/*
 * Authority gate, exact dispatcher coverage over every AP_* index.
 * Master-chain params 9-22 are structurally rejected on the register
 * path -- a register write is never issued by the scene module. The
 * upload path closes once the control plane holds authority. Runs
 * first, against the pre-init module state.
 */
static void test_gate_before_init(void)
{
	uint32_t p;

	check(audio_scene_authority_active() == 0,
		"authority inactive before init", NULL);
	for (p = 0; p < ZZ_NUM_AUDIO_PARAMS; p++) {
		int expect = (p >= AP_DSP_SET_LOWPASS) &&
			(p <= AP_DSP_SET_STEREO_VOLUME);
		check(audio_scene_register_write_blocked(p) == expect,
			"pre-init gate rejects master chain only",
			fmt("param %lu blocked=%d expected=%d",
				(unsigned long)p,
				audio_scene_register_write_blocked(p),
				expect));
	}
	check(audio_scene_register_write_blocked(AP_DSP_UPLOAD) == 0,
		"upload open before authority claimed", NULL);
}

static void test_gate_after_init(void)
{
	uint32_t p;

	audio_scene_init();
	check(audio_scene_authority_active() == 1,
		"authority active after init", NULL);
	check(audio_scene_register_write_blocked(AP_DSP_UPLOAD) == 1,
		"upload rejected while authority active", NULL);
	check(audio_scene_register_write_blocked(AP_TX_BUF_OFFS_LO) == 0,
		"tx buffer offset stays on legacy path", NULL);
	check(audio_scene_register_write_blocked(AP_RX_BUF_OFFS_LO) == 0,
		"rx buffer offset stays on legacy path", NULL);
	check(audio_scene_register_write_blocked(AP_DSP_PROG_OFFS_LO) == 0,
		"dsp program offset stays on legacy path", NULL);
	check(audio_scene_register_write_blocked(AP_DSP_PARAM_OFFS_LO) == 0,
		"dsp param offset stays on legacy path", NULL);
	for (p = AP_DSP_SET_LOWPASS; p <= AP_DSP_SET_STEREO_VOLUME; p++) {
		check(audio_scene_register_write_blocked(p) == 1,
			"post-init gate rejects every master-chain index",
			fmt("param %lu", (unsigned long)p));
	}
}

/*
 * The accepted path: for every rejected register index, the scene
 * module produces the corresponding verified DSP write. Apply order
 * is the KTD7 commit sequence (U4): fade vol/pan to zero, LPF, EQ
 * bands 0..9, prefactor, staged mixer legs, volume restore.
 */
static void test_accepted_paths_route_through_scene(void)
{
	struct audio_scene_def def;
	int i;
	int kind = 0, a = 0, b = 0;
	int ok;

	unity_scene(&def);
	def.lpf_hz = 20000;
	for (i = 0; i < AUDIO_SCENE_EQ_BANDS; i++)
		def.eq[i] = (uint8_t)(30 + 2 * i); /* 30..48, no boost */
	def.prefactor = 50;
	def.volume = 90;
	def.pan = 30;

	audio_scene_init();
	clear_writes();
	check(audio_scene_write(6, &def) == 0, "scene write slot 6", NULL);
	check(write_count == 0, "write to inactive slot touches no DSP",
		fmt("writes=%d", write_count));

	/* AP_DSP_SET_LOWPASS (9) */
	check(audio_scene_select(6) == 0, "scene select applies", NULL);
	pump_scene();
	ok = log_at(0, &kind, &a, &b) && kind == WRITE_VOLPAN &&
		a == 0 && b == 30;
	check(ok, "commit fades output to zero first",
		fmt("kind=%d vol=%d pan=%d", kind, a, b));
	ok = log_at(1, &kind, &a, &b) && kind == WRITE_LPF && a == 20000;
	check(ok, "commit writes LPF (AP_DSP_SET_LOWPASS)",
		fmt("kind=%d f0=%d", kind, a));
	/* AP_DSP_SET_EQ_BAND1..10 (12..21) */
	for (i = 0; i < AUDIO_SCENE_EQ_BANDS; i++) {
		ok = log_at(2 + i, &kind, &a, &b) && kind == WRITE_EQ &&
			a == i && b == 30 + 2 * i;
		check(ok, "commit writes EQ bands in order",
			fmt("entry %d kind=%d band=%d gain=%d", 2 + i, kind,
				a, b));
	}
	/* AP_DSP_SET_PREFACTOR (11) */
	ok = log_at(12, &kind, &a, &b) && kind == WRITE_PREF && a == 50;
	check(ok, "commit writes prefactor (AP_DSP_SET_PREFACTOR)",
		fmt("kind=%d pre=%d", kind, a));
	/* AP_DSP_SET_VOLUMES (10): staged baseline mixer legs */
	ok = log_at(13, &kind, &a, &b) && kind == WRITE_MIXER &&
		a == 128 && b == 64;
	check(ok, "commit stages mixer legs (AP_DSP_SET_VOLUMES)",
		fmt("kind=%d v1=%d v2=%d", kind, a, b));
	/* AP_DSP_SET_STEREO_VOLUME (22): the fade's restore */
	ok = log_at(14, &kind, &a, &b) && kind == WRITE_VOLPAN &&
		a == 90 && b == 30;
	check(ok, "commit restores output volume/pan last "
		"(AP_DSP_SET_STEREO_VOLUME)",
		fmt("kind=%d vol=%d pan=%d", kind, a, b));
	check(write_count == 15,
		"commit is exactly 15 ordered writes "
		"(fade, params, restore)",
		fmt("writes=%d", write_count));
	check(audio_scene_gain_reduction_events() == 0,
		"within-boundary apply emits no event", NULL);

	/* Invalid slots are refused without touching the DSP. */
	clear_writes();
	check(audio_scene_select(AUDIO_SCENE_COUNT) == -1,
		"select rejects out-of-range slot", NULL);
	check(audio_scene_write(AUDIO_SCENE_COUNT, &def) == -1,
		"write rejects out-of-range slot", NULL);
	check(audio_scene_get(AUDIO_SCENE_COUNT) == NULL,
		"get rejects out-of-range slot", NULL);
	def.eq[0] = 101;
	check(audio_scene_write(1, &def) == -1,
		"write rejects out-of-range EQ gain", NULL);
	def.eq[0] = 50;
	def.volume = 101;
	check(audio_scene_write(1, &def) == -1,
		"write rejects out-of-range volume", NULL);
	def.volume = 100;
	def.lpf_hz = 24000;
	check(audio_scene_write(1, &def) == -1,
		"write rejects out-of-range LPF", NULL);
	check(write_count == 0, "rejected writes touch no DSP state",
		fmt("writes=%d", write_count));

	/* A verified-write failure aborts the machine asynchronously: the
	 * dispatch still returns immediately and issues no I2C itself. */
	unity_scene(&def);
	fail_next_write = 1;
	check(audio_scene_write(4, &def) == 0,
		"write to inactive slot needs no DSP", NULL);
	check(audio_scene_select(4) == 0,
		"select dispatch returns immediately", NULL);
	check(write_count == 0,
		"failing commit issues no writes before poll",
		fmt("writes=%d", write_count));
	pump_scene();
	fail_next_write = 0;
	check(audio_scene_active_index() == 4,
		"failed commit keeps the slot active for retry", NULL);
	check(last_write(WRITE_VOLPAN, &a, &b) && a == 100 && b == 50,
		"aborted machine best-effort restores the output volume",
		fmt("vol=%d pan=%d", a, b));
	clear_writes();
	check(audio_scene_select(4) == 0, "retry accepted", NULL);
	pump_scene();
	check(write_count == 15,
		"retry applies the full 15-write sequence",
		fmt("writes=%d", write_count));
}

/*
 * Default baseline summed (128+64 = 192) against the enforced
 * boundary (192) with a unity scene: exactly at the boundary passes
 * without clamp or event.
 */
static void test_staging_at_boundary(void)
{
	int a = -1, b = -1;

	audio_scene_init();
	clear_writes();
	check(audio_scene_select(0) == 0, "apply default scene", NULL);
	pump_scene();
	check(last_write(WRITE_MIXER, &a, &b) && a == 128 && b == 64,
		"at-boundary composition passes at full legs",
		fmt("v1=%d v2=%d", a, b));
	check(audio_scene_gain_reduction_events() == 0,
		"at-boundary composition emits no event", NULL);
}

static void test_staging_one_step_over(void)
{
	struct audio_scene_trim_result result;
	const struct audio_scene_gain_event *event;
	int a = -1, b = -1;

	audio_scene_init();
	audio_scene_select(0);
	pump_scene();
	clear_writes();
	memset(&result, 0, sizeof(result));
	check(audio_scene_trim_submit(AUDIO_SCENE_OWNER_AHI, 1, 0,
			&result) == 0, "trim submit accepted for processing",
		NULL);
	check(result.bounded == 1, "one-step-over trim is bounded", NULL);
	check(result.mixer_paula == 128 && result.mixer_ax == 63,
		"bounded trim applied legs",
		fmt("v1=%u v2=%u", result.mixer_paula, result.mixer_ax));
	check(near(result.trim_bound, 0.0),
		"bound reports zero headroom at the boundary",
		fmt("trim_bound=%.3f", result.trim_bound));
	check(last_write(WRITE_MIXER, &a, &b) && a == 128 && b == 63,
		"clamped mixer write issued",
		fmt("v1=%d v2=%d", a, b));
	check(audio_scene_gain_reduction_events() == 1,
		"exactly one gain-reduction event",
		fmt("events=%lu",
			(unsigned long)audio_scene_gain_reduction_events()));
	event = audio_scene_last_gain_reduction();
	check(event != NULL && near(event->requested, 193.0) &&
		near(event->applied, 191.0) &&
		near(event->boundary, AUDIO_SCENE_ENFORCED_BOUNDARY),
		"event reports requested/applied/boundary",
		event ? fmt("req=%.3f applied=%.3f boundary=%.3f",
			event->requested, event->applied, event->boundary)
			: "no event");

	audio_scene_trim_release(AUDIO_SCENE_OWNER_AHI);
	check(last_write(WRITE_MIXER, &a, &b) && a == 128 && b == 64,
		"release restores neutral legs",
		fmt("v1=%d v2=%d", a, b));
	check(audio_scene_gain_reduction_events() == 1,
		"release adds no event", NULL);
}

/*
 * A scene whose own master-chain level exceeds the boundary -- with
 * the neutral baseline and no requester -- clamps on apply.
 */
static void test_scene_alone_clamps(void)
{
	struct audio_scene_def def;
	const struct audio_scene_gain_event *event;
	int a = -1, b = -1;
	double boost = DB12_TO_LIN(12.0);

	unity_scene(&def);
	def.prefactor = 100; /* +12 dB */
	def.volume = 100;
	audio_scene_init();
	clear_writes();
	check(audio_scene_write(2, &def) == 0 &&
		audio_scene_select(2) == 0, "apply boosting scene", NULL);
	pump_scene();
	check(audio_scene_gain_reduction_events() == 1,
		"scene-alone clamp emits exactly one event",
		fmt("events=%lu",
			(unsigned long)audio_scene_gain_reduction_events()));
	event = audio_scene_last_gain_reduction();
	check(event != NULL &&
		near(event->requested, 192.0 * boost) &&
		event->applied <= AUDIO_SCENE_ENFORCED_BOUNDARY &&
		near(event->boundary, AUDIO_SCENE_ENFORCED_BOUNDARY),
		"scene-alone event within boundary",
		event ? fmt("req=%.3f applied=%.3f", event->requested,
			event->applied) : "no event");
	check(last_write(WRITE_VOLPAN, &a, &b) && a < 100 && b == 50,
		"applied output volume reduced, pan kept",
		fmt("vol=%d pan=%d", a, b));
	check(last_write(WRITE_MIXER, &a, &b) && a == 128 && b == 64,
		"baseline legs unclamped after volume reduction",
		fmt("v1=%d v2=%d", a, b));
	check(audio_scene_gain_reduction_events() == 1,
		"mixer staging after scene clamp adds no second event", NULL);
	check(audio_scene_get(2) != NULL &&
		audio_scene_get(2)->volume == 100,
		"stored scene definition unchanged by the clamp", NULL);
}

/*
 * Worst-case positive EQ band gain participates in staging: a boosted
 * EQ scene at otherwise-safe prefactor and volume still clamps.
 */
static void test_eq_boost_clamps(void)
{
	struct audio_scene_def def;
	int a = -1, b = -1;
	unity_scene(&def);
	def.eq[0] = 100; /* +12 dB on band 1, prefactor/volume unity */
	audio_scene_init();
	clear_writes();
	check(audio_scene_write(3, &def) == 0 &&
		audio_scene_select(3) == 0, "apply EQ-boosted scene", NULL);
	pump_scene();
	check(audio_scene_gain_reduction_events() == 1,
		"EQ-boosted scene clamps with one event",
		fmt("events=%lu",
			(unsigned long)audio_scene_gain_reduction_events()));
	check(last_write(WRITE_VOLPAN, &a, &b) && a < 100,
		"EQ boost reduces applied volume", fmt("vol=%d", a));
	check(count_writes(WRITE_EQ) == AUDIO_SCENE_EQ_BANDS &&
		last_write(WRITE_MIXER, &a, &b) && a == 128 && b == 64,
		"full EQ set committed, mixer staged",
		fmt("eq=%d v1=%d v2=%d", count_writes(WRITE_EQ), a, b));
}

/*
 * Baseline + trim composition: raising the baseline shrinks the trim
 * headroom; the same trim that was accepted becomes bounded and the
 * reported bound reflects the baseline.
 */
static void test_baseline_trim_composition(void)
{
	struct audio_scene_def def;
	struct audio_scene_trim_result result;
	const struct audio_scene_gain_event *event;
	int a = -1, b = -1;

	unity_scene(&def);
	def.volume = 50; /* scene linear gain 0.5 -> summed bound 384 */
	audio_scene_init();
	audio_scene_write(5, &def);
	audio_scene_select(5);
	pump_scene();

	memset(&result, 0, sizeof(result));
	check(audio_scene_trim_submit(AUDIO_SCENE_OWNER_MHI, 50, 50,
			&result) == 0 && result.bounded == 0 &&
		result.mixer_paula == 178 && result.mixer_ax == 114,
		"trim accepted at low baseline",
		fmt("bounded=%u v1=%u v2=%u", result.bounded,
			result.mixer_paula, result.mixer_ax));
	check(near(result.trim_bound, 192.0),
		"trim headroom at low baseline",
		fmt("trim_bound=%.3f", result.trim_bound));
	audio_scene_trim_release(AUDIO_SCENE_OWNER_MHI);

	audio_scene_set_baseline(200, 100);
	pump_scene();
	check(audio_scene_baseline_paula() == 200 &&
		audio_scene_baseline_ax() == 100, "baseline stored", NULL);
	check(last_write(WRITE_MIXER, &a, &b) && a == 200 && b == 100,
		"baseline restaged without clamp",
		fmt("v1=%d v2=%d", a, b));
	check(audio_scene_gain_reduction_events() == 0,
		"baseline within boundary emits no event", NULL);

	memset(&result, 0xFF, sizeof(result));
	check(audio_scene_trim_submit(AUDIO_SCENE_OWNER_MHI, 50, 50,
			&result) == 0 && result.bounded == 1 &&
		result.mixer_paula == 240 && result.mixer_ax == 144,
		"same trim bounded at high baseline",
		fmt("bounded=%u v1=%u v2=%u", result.bounded,
			result.mixer_paula, result.mixer_ax));
	check(near(result.trim_bound, 84.0),
		"reported bound reflects the raised baseline",
		fmt("trim_bound=%.3f", result.trim_bound));
	check(audio_scene_gain_reduction_events() == 1,
		"baseline-shifted bound emits one event", NULL);
	event = audio_scene_last_gain_reduction();
	check(event != NULL && near(event->requested, 200.0) &&
		near(event->applied, 192.0),
		"event reports composed levels across baseline and trim",
		event ? fmt("req=%.3f applied=%.3f", event->requested,
			event->applied) : "no event");
}

/*
 * Boot and warm-reset apply: scene writes follow the ADAU init
 * defaults and land before the request loop could service an owner
 * (the gate stays closed throughout), and the order repeats after a
 * warm reset with trims torn down.
 */
static void test_boot_apply_order(void)
{
	int kind = 0, a = 0, b = 0;
	int i;
	int ok;
	int base;

	audio_scene_init();
	clear_writes();

	/* audio_adau_init(1) writes its defaults: LPF 23900, mixer
	 * 128/64 (ax.c:753-755). Simulated through the same seam. */
	audio_adau_set_lpf_params(23900);
	audio_adau_set_mixer_vol(128, 64);
	check(audio_scene_apply_after_dsp_init() == 0,
		"apply after DSP init succeeds", NULL);

	check(write_count == 17, "init defaults plus one scene commit",
		fmt("writes=%d", write_count));
	ok = log_at(0, &kind, &a, &b) && kind == WRITE_LPF && a == 23900 &&
		log_at(1, &kind, &a, &b) && kind == WRITE_MIXER &&
		a == 128 && b == 64;
	check(ok, "ADAU init defaults precede scene writes", NULL);
	base = 2;
	ok = log_at(base, &kind, &a, &b) && kind == WRITE_VOLPAN &&
		a == 0 && b == 50;
	ok = ok && log_at(base + 1, &kind, &a, &b) &&
		kind == WRITE_LPF && a == 23900;
	for (i = 0; ok && i < AUDIO_SCENE_EQ_BANDS; i++)
		ok = log_at(base + 2 + i, &kind, &a, &b) &&
			kind == WRITE_EQ && a == i && b == 50;
	ok = ok && log_at(base + 12, &kind, &a, &b) &&
		kind == WRITE_PREF && a == 50;
	ok = ok && log_at(base + 13, &kind, &a, &b) &&
		kind == WRITE_MIXER && a == 128 && b == 64;
	ok = ok && log_at(base + 14, &kind, &a, &b) &&
		kind == WRITE_VOLPAN && a == 100 && b == 50;
	check(ok, "scene commit: fade, LPF, EQ 0..9, prefactor, mixer, "
		"restore", NULL);
	check(audio_scene_gain_reduction_events() == 0,
		"telemetry counters reset with DSP re-init", NULL);
	check(audio_scene_authority_active() == 1 &&
		audio_scene_register_write_blocked(AP_DSP_UPLOAD) == 1 &&
		audio_scene_register_write_blocked(
			AP_DSP_SET_STEREO_VOLUME) == 1,
		"gate closed before owner service", NULL);

	/* Owner activity, then an Amiga warm reset: DSP defaults return,
	 * the active scene re-applies, and the owner's trim is gone. */
	check(audio_scene_select(1) == 0, "operator selects scene 1", NULL);
	{
		struct audio_scene_trim_result result;
		memset(&result, 0, sizeof(result));
		check(audio_scene_trim_submit(AUDIO_SCENE_OWNER_AHI, 10, 10,
				&result) == 0 && result.bounded == 0,
			"owner trim before warm reset", NULL);
	}
	audio_adau_set_lpf_params(23900);
	audio_adau_set_mixer_vol(128, 64);
	check(audio_scene_apply_after_dsp_init() == 0,
		"warm-reset re-apply succeeds", NULL);
	base = write_count - 15;
	ok = log_at(base, &kind, &a, &b) && kind == WRITE_VOLPAN &&
		a == 0 && b == 50;
	ok = ok && log_at(base + 1, &kind, &a, &b) &&
		kind == WRITE_LPF && a == 23900;
	for (i = 0; ok && i < AUDIO_SCENE_EQ_BANDS; i++)
		ok = log_at(base + 2 + i, &kind, &a, &b) &&
			kind == WRITE_EQ && a == i && b == 50;
	ok = ok && log_at(base + 12, &kind, &a, &b) &&
		kind == WRITE_PREF && a == 50;
	ok = ok && log_at(base + 13, &kind, &a, &b) &&
		kind == WRITE_MIXER && a == 128 && b == 64;
	ok = ok && log_at(base + 14, &kind, &a, &b) &&
		kind == WRITE_VOLPAN && a == 80 && b == 50;
	check(ok, "warm reset re-applies active scene with neutral trim",
		fmt("base=%d writes=%d", base, write_count));
	check(audio_scene_gain_reduction_events() == 0,
		"warm reset resets telemetry counters", NULL);
}

static void test_trim_lifecycle(void)
{
	struct audio_scene_trim_result result;
	int a = -1, b = -1;

	audio_scene_init();
	audio_scene_select(0);
	pump_scene();
	clear_writes();


	/* Negative trim always fits: it lowers the composed level. */
	memset(&result, 0, sizeof(result));
	check(audio_scene_trim_submit(AUDIO_SCENE_OWNER_SDK, -20, -20,
			&result) == 0 && result.bounded == 0 &&
		result.mixer_paula == 108 && result.mixer_ax == 44,
		"negative trim accepted",
		fmt("bounded=%u v1=%u v2=%u", result.bounded,
			result.mixer_paula, result.mixer_ax));
	check(last_write(WRITE_MIXER, &a, &b) && a == 108 && b == 44,
		"accepted trim applied to mixer legs",
		fmt("v1=%d v2=%d", a, b));

	memset(&result, 0, sizeof(result));
	check(audio_scene_trim_submit(AUDIO_SCENE_OWNER_SDK, 10, 0,
			&result) == 0 && result.bounded == 1,
		"positive trim beyond headroom bounded", NULL);
	check(audio_scene_gain_reduction_events() == 1,
		"bounded trim emits one event", NULL);

	/* Unknown owners are refused without touching the DSP. */
	clear_writes();
	check(audio_scene_trim_submit(AUDIO_SCENE_OWNER_NONE, 10, 10,
			&result) == -1, "trim rejects the NONE owner", NULL);
	check(audio_scene_trim_submit(AUDIO_SCENE_OWNER_SLOTS, 10, 10,
			&result) == -1, "trim rejects out-of-range owner",
		NULL);
	audio_scene_trim_release(AUDIO_SCENE_OWNER_NONE);
	check(write_count == 0,
		"refused or unknown-owner trim touches no DSP state", NULL);

	audio_scene_trim_release(AUDIO_SCENE_OWNER_SDK);
	check(last_write(WRITE_MIXER, &a, &b) && a == 128 && b == 64,
		"release resets trim to neutral baseline", NULL);
}

/*
 * Staged-edit commit failure: the staging is consumed only on
 * success, so a failed write sequence leaves the draft pending (and
 * the live tables untouched) and a retry re-issues the full commit
 * instead of returning OK with zero writes.
 */
static void test_commit_failure_keeps_staging(void)
{
	int a = -1, b = -1;

	audio_scene_init();
	clear_writes();

	check(audio_scene_stage_param(0, SDK_AUDIO_SCENE_PARAM_VOLUME, 70)
		== 0, "stage a volume edit", NULL);
	fail_next_write = 1;
	check(audio_scene_commit_staged(0) == 0,
		"staged commit accepted (applies asynchronously)", NULL);
	pump_scene();
	check(audio_scene_get(0)->volume == 100,
		"live scene untouched by the failed commit",
		fmt("volume=%u", audio_scene_get(0)->volume));

	fail_next_write = 0;
	clear_writes();
	check(audio_scene_commit_staged(0) == 0,
		"retry after failure accepted", NULL);
	pump_scene();
	check(write_count == 15,
		"retry re-issues the full write sequence",
		fmt("writes=%d", write_count));
	check(audio_scene_get(0)->volume == 70,
		"retried commit lands the staged edit", NULL);
	check(last_write(WRITE_VOLPAN, &a, &b) && a == 70 && b == 50,
		"retry restores the staged output volume",
		fmt("vol=%d pan=%d", a, b));

	/* A staged baseline joins the same rule: failure restores it. */
	check(audio_scene_stage_param(0, SDK_AUDIO_SCENE_PARAM_BASELINE,
		SDK_AUDIO_BALANCE_PACK(150, 40)) == 0,
		"stage a baseline edit", NULL);
	fail_next_write = 1;
	check(audio_scene_commit_staged(0) == 0,
		"baseline commit accepted (applies asynchronously)", NULL);
	pump_scene();
	check(audio_scene_baseline_paula() == 128 &&
		audio_scene_baseline_ax() == 64,
		"baseline untouched by the failed commit", NULL);
	fail_next_write = 0;
	clear_writes();
	check(audio_scene_commit_staged(0) == 0,
		"baseline retry accepted", NULL);
	pump_scene();
	check(audio_scene_baseline_paula() == 150 &&
		audio_scene_baseline_ax() == 40,
		"retried baseline commit lands", NULL);
}

/*
 * Mid-commit verified-write failure: by the time the EQ safeload
 * group runs, the chain is already faded down; the failure path must
 * best-effort restore the resolved output volume so the DAC is not
 * left silent.
 */
static void test_apply_failure_restores_volume(void)
{
	int kind = 0, a = 0, b = 0;

	audio_scene_init();
	clear_writes();

	fail_eq_band = 3; /* fail inside the EQ safeload group */
	check(audio_scene_select(1) == 0,
		"select dispatch accepted despite the upcoming failure",
		NULL);
	check(write_count == 0,
		"failing commit issues no writes before poll",
		fmt("writes=%d", write_count));
	pump_scene();
	check(write_count == 7,
		"failure path stops at the failed write plus one restore",
		fmt("writes=%d", write_count));
	check(log_at(5, &kind, &a, &b) && kind == WRITE_EQ && a == 3,
		"EQ band 3 write failed mid-sequence", NULL);
	check(log_at(6, &kind, &a, &b) && kind == WRITE_VOLPAN &&
		a == 80 && b == 50,
		"restore write follows the failure immediately",
		fmt("kind=%d vol=%d pan=%d", kind, a, b));
}

/*
 * P1 immediate-return contract: the dispatch entry points start the
 * commit machine and return before any I2C; only audio_scene_poll
 * issues DSP writes, one setter call per call.
 */
static void test_dispatch_does_not_block(void)
{
	struct audio_scene_def def;

	audio_scene_init();
	clear_writes();

	check(audio_scene_select(3) == 0,
		"select dispatch returns immediately", NULL);
	check(write_count == 0,
		"select issued zero DSP writes before any poll",
		fmt("writes=%d", write_count));
	pump_scene();
	check(write_count == 15,
		"poll drains the machine into one full commit sequence",
		fmt("writes=%d", write_count));

	unity_scene(&def);
	def.volume = 77;
	clear_writes();
	check(audio_scene_write(3, &def) == 0,
		"active-scene write dispatch returns immediately", NULL);
	check(write_count == 0,
		"scene write issued zero DSP writes before any poll",
		fmt("writes=%d", write_count));
	pump_scene();
	check(write_count == 15,
		"poll drains the write commit",
		fmt("writes=%d", write_count));

	clear_writes();
	check(audio_scene_set_baseline(140, 70) == 0,
		"baseline dispatch returns immediately", NULL);
	check(write_count == 0,
		"baseline change issued zero DSP writes before any poll",
		fmt("writes=%d", write_count));
	pump_scene();
	check(write_count == 15,
		"poll drains the baseline commit",
		fmt("writes=%d", write_count));
}

/*
 * Restore-write failure: the sequence's last write fails; the machine
 * retries it once best-effort and finishes as a FAILED commit, so the
 * staged rollback still applies and the staging survives for a retry.
 */
static void test_restore_failure_keeps_staging(void)
{
	audio_scene_init();
	clear_writes();

	check(audio_scene_stage_param(0, SDK_AUDIO_SCENE_PARAM_VOLUME, 70)
		== 0, "stage a volume edit", NULL);
	fail_volpan_restore = 1;
	check(audio_scene_commit_staged(0) == 0,
		"staged commit accepted (applies asynchronously)", NULL);
	pump_scene();
	check(audio_scene_get(0)->volume == 100,
		"restore-write failure rolls the staging back",
		fmt("volume=%u", audio_scene_get(0)->volume));
	check(count_writes(WRITE_VOLPAN) == 3,
		"fade + failed restore + one best-effort retry",
		fmt("volpan=%d", count_writes(WRITE_VOLPAN)));

	fail_volpan_restore = 0;
	check(audio_scene_commit_staged(0) == 0, "retry accepted", NULL);
	pump_scene();
	check(audio_scene_get(0)->volume == 70,
		"retried commit lands the staged edit",
		fmt("volume=%u", audio_scene_get(0)->volume));
}

int main(void)
{
	/* Order matters: the gate test observes the pre-init state. */
	test_gate_before_init();
	test_gate_after_init();
	test_accepted_paths_route_through_scene();
	test_staging_at_boundary();
	test_staging_one_step_over();
	test_scene_alone_clamps();
	test_eq_boost_clamps();
	test_baseline_trim_composition();
	test_boot_apply_order();
	test_trim_lifecycle();
	test_commit_failure_keeps_staging();
	test_apply_failure_restores_volume();
	test_dispatch_does_not_block();
	test_restore_failure_keeps_staging();

	if (failures == 0) {
		printf("audio_scene_test: all tests passed\n");
		return 0;
	}
	printf("audio_scene_test: %d failure(s)\n", failures);
	return 1;
}
