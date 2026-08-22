/*
 * Firmware-authoritative audio control plane arbiter (plan U2, KTD2):
 * scene state, split authority, gain staging against the enforced
 * saturation boundary, and -- since U3 -- the per-direction metering
 * accumulators with coherent snapshots (KTD3). Since U4 it also owns
 * the staged-edit drafts and the single KTD7 glitch-free commit path
 * (fade -> ordered verified writes -> restore) that every
 * master-chain change funnels through; that commit runs as an
 * incremental state machine driven by audio_scene_poll(), one DSP
 * setter call per call, so the dispatch path never blocks on the
 * ~170-transaction I2C sequence. Live edits (staged SCENE_WRITE
 * commits, baseline changes) run that machine differentially: only
 * the parameters that differ from the last applied state, with no
 * fade, so a parameter drag costs one setter call per change.
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

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "audio_dsp_gain.h"
#include "audio_scene.h"
#include "sdk_mailbox.h"
#include "zz_config.h"
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
 * Fixed scene slots. Every default composes to
 * at or below the enforced boundary with the default baseline summed
 * (192): the worst defaults apply 192 * prefactor * volume * EQ-boost
 * <= 192. Slot 0 is today's power-on default (LPF 23900, unity EQ,
 * unity prefactor, full volume) and composes exactly at the boundary.
 * Names are user labels ("Scene N" is the built-in default) and never
 * join the DSP write set.
 */
static const struct audio_scene_def default_scenes[AUDIO_SCENE_COUNT] = {
	{ 23900, { 50, 50, 50, 50, 50, 50, 50, 50, 50, 50 }, 50, 100, 50, "Scene 1" },
	{ 23900, { 50, 50, 50, 50, 50, 50, 50, 50, 50, 50 }, 50, 80, 50, "Scene 2" },
	{ 16000, { 55, 55, 50, 50, 50, 50, 50, 50, 50, 50 }, 50, 75, 50, "Scene 3" },
	{ 18000, { 50, 50, 50, 50, 50, 50, 55, 55, 50, 50 }, 50, 75, 50, "Scene 4" },
	{ 23900, { 50, 50, 50, 50, 50, 50, 50, 50, 50, 50 }, 60, 70, 50, "Scene 5" },
	{ 12000, { 45, 45, 50, 50, 50, 50, 50, 50, 45, 45 }, 50, 90, 50, "Scene 6" },
	{ 23900, { 50, 50, 50, 50, 50, 50, 50, 50, 50, 50 }, 55, 75, 50, "Scene 7" },
	{ 23900, { 50, 50, 50, 50, 50, 50, 50, 50, 50, 50 }, 50, 60, 50, "Scene 8" },
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

struct scene_staging {
	struct audio_scene_def draft;
	int has_draft; /* params staged for this scene, uncommitted */
	/* NAME accumulator state (the label is staged chunk by chunk):
	 * chunks counts two-char chunks already written into draft.name,
	 * open is set once the first chunk cleared the name, and closed
	 * is set by the terminator chunk. */
	uint8_t name_chunks;
	uint8_t name_open;
	uint8_t name_closed;
};

static struct scene_staging staging[AUDIO_SCENE_COUNT];
static uint8_t staged_baseline_paula;
static uint8_t staged_baseline_ax;
static int baseline_staged;

/* ---- incremental commit machine (P1: the dispatch path never
 * blocks on I2C) ---- */

/*
 * The KTD7 write sequence (fade -> LPF -> ten EQ bands -> prefactor
 * -> mixer legs -> restore) is ~170 verified I2C transactions at
 * 100 kHz. Running it inline in the mailbox dispatch stalled core 0
 * long enough for ZZ9KCall clients to time out. The commit is
 * therefore an incremental state machine: the dispatch entry
 * points validate, update the live tables, start the machine and
 * return immediately; audio_scene_poll() -- called once per main
 * loop iteration -- advances it by at most ONE setter call, so the
 * steps interleave with the per-period AHI traffic.
 * The machine works on a snapshot taken at start (scene definition,
 * resolved volume, composed mixer stage), so table edits that arrive
 * mid-flight can never tear the running sequence: they coalesce into
 * a pending target that a fresh machine applies when this one
 * reaches its terminal state.
 *
 * Differential mode (the fast path): a live single-parameter edit --
 * a staged SCENE_WRITE commit or a direct baseline change -- runs
 * the same machine with a todo list of only the parameters that
 * differ from the last successfully applied state: no fade, no
 * restore, one setter per changed parameter. Scene selects and DSP
 * re-inits keep the full fade-commit-restore sequence above (they
 * replace the whole chain at once), so a slider drag no longer
 * multiplies into fifteen-write machines that starve the Zorro
 * service loop.
 */
enum commit_phase {
	COMMIT_FADE,         /* verified fade of the output to zero */
	COMMIT_FAST,         /* differential: the changed parameters only */
	COMMIT_LPF,
	COMMIT_EQ,           /* eq_band in flight, 0..9 */
	COMMIT_PREF,
	COMMIT_MIXER,        /* snapshot mixer legs */
	COMMIT_RESTORE,      /* resolved output volume back up */
	COMMIT_ABORT_RESTORE, /* best-effort restore after a failed step */
	COMMIT_DONE           /* terminal: finish (and chain a pending) */
};

/* Set while a machine's write sequence is between its first fade and
 * its terminal state (DONE or abort), including the finish jump into
 * a coalesced follow-up machine. */
static int commit_in_progress;

/* Guard against a poll() re-entered from inside a setter (the same
 * nested-dispatch seam the tests exercise): the outer step owns the
 * phase transitions. */
static int commit_stepping;


/*
 * Coalesced request: a scene switch (or a table edit) that arrived
 * while a machine was running. When the machine reaches its terminal
 * state, a fresh machine starts toward pending_target unless it
 * equals the index just applied AND no table changed since that
 * machine started (a redundant re-select coalesces to nothing). The
 * follow-up runs the mode its origin asked for.
 */
static int pending_valid;
static uint8_t pending_target;
static int pending_dirty; /* live tables changed mid-machine */
static int pending_fast;  /* the coalesced follow-up is differential */

/*
 * The last state a machine left on the DSP: the scene definition, the
 * resolved output volume/pan it ended at, and (last_mixer_stage
 * below) the mixer legs -- the baseline the differential commit
 * diffs against. Updated ONLY when a machine completes successfully;
 * a failed machine leaves it at the pre-failure state on purpose, so
 * the retry rewrites the full diff. Invalid until the first success
 * (a differential commit then writes everything).
 */
static struct audio_scene_def last_applied_scene;
static int last_applied_vol;  /* resolved (possibly reduced) volume */
static int last_applied_pan;
static int last_applied_valid;

/* One differential todo entry: a DSP setter call with its arguments. */
enum fast_op {
	FAST_LPF,    /* audio_adau_set_lpf_params(a) */
	FAST_EQ,     /* audio_adau_set_eq_gain(a, b) */
	FAST_PREF,   /* audio_adau_set_prefactor(a) */
	FAST_MIXER_P, /* audio_adau_set_mixer_leg(0, a) - Paula */
	FAST_MIXER_A, /* audio_adau_set_mixer_leg(1, a) - AX */
	FAST_VOLPAN_L, /* audio_adau_set_vol_pan_side(0, a, b) */
	FAST_VOLPAN_R  /* audio_adau_set_vol_pan_side(1, a, b) */
};

struct fast_step {
	int op;
	int a;
	int b;
	int sub; /* EQ substep fan-out index */
};

/*
 * Staged-commit rollback (U4 review fix): commit_staged takes the
 * staged edits optimistically because the machine reads the live
 * tables; if the applying machine ultimately fails, these pre-commit
 * copies are restored so the staging survives and a retry re-issues
 * the whole sequence. Cleared when the machine that began after the
 * consumption completes successfully.
 */
struct staged_rollback {
	int valid;
	uint8_t scene_index;       /* slot whose scene/staging was consumed */
	int scene_saved;
	struct audio_scene_def scene;
	struct scene_staging staging;
	int baseline_saved;
	uint8_t baseline_paula;
	uint8_t baseline_ax;
	int baseline_staged;
};

static struct staged_rollback rollback;

/* Synchronously drain the machine to its terminal state (bounded by
 * the fixed per-machine step count; nothing can enqueue new work
 * while the caller holds the thread). Boot/reset apply and scene
 * save use this. */
static void commit_flush(void)
{
	int guard = 0;

	while (audio_scene_poll() && ++guard < 1000)
		;
}

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
			double linear = audio_db_to_linear(db);
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
	double chain_linear; /* master chain at unity volume */
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
	double baseline = baseline_sum_linear();
	double level = baseline * chain * volume_linear;

	out->applied_volume = scene->volume;
	out->linear = volume_linear;
	out->chain_linear = chain;
	out->reduced = 0;
	out->requested_level = level;
	out->applied_level = level;
	if (level > AUDIO_SCENE_ENFORCED_BOUNDARY) {
		double allowed = AUDIO_SCENE_ENFORCED_BOUNDARY /
			(baseline * chain);
		if (allowed > 1.0)
			allowed = 1.0;
		out->applied_volume = (int)(allowed * 100.0);
		out->linear = ((double)out->applied_volume) / 100.0;
		out->reduced = 1;
		out->applied_level = baseline * chain * out->linear;
	}
}

struct mixer_stage {
	uint8_t bounded;
	uint8_t paula; /* applied mixer legs (absolute) */
	uint8_t ax;
	double trim_bound;  /* summed-trim headroom for this scene+baseline */
	double requested;   /* composed level when bounded */
	double applied;     /* composed level after reduction */
};

/* Last applied mixer composition (trim reporting, R3). */
static struct mixer_stage last_mixer_stage;

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
	compute_mixer_stage(vol.chain_linear * vol.linear, stage);
	if (stage->bounded)
		emit_gain_reduction(stage->requested, stage->applied);
	last_mixer_stage = *stage;
	return audio_adau_set_mixer_vol(stage->paula, stage->ax);
}
/* The running machine, snapshotting the volume_resolution and
 * mixer_stage types defined above. In differential mode it carries a
 * precomputed todo list instead of the fade/restore envelope. */
struct commit_machine {
	enum commit_phase phase;
	int eq_band;
	uint8_t scene_index;           /* slot this machine applies */
	struct audio_scene_def scene;  /* snapshot: tables may change */
	struct volume_resolution vol;  /* resolved at machine start */
	struct mixer_stage stage;      /* composed at machine start */
	int failed;
	int owns_rollback; /* this machine applies a staged commit's tables */
	int fast;          /* differential mode: todo list, no fade */
	struct fast_step todo[224]; /* worst: full diff + 99-step ramp */
	int todo_count;
	int todo_next;     /* next todo entry to issue */
};

static struct commit_machine commit;

/* Record the state a successful machine leaves on the DSP: the scene
 * definition, its resolved output volume/pan, and the mixer legs --
 * the baseline the next differential commit diffs against. */
static void fast_record_applied(const struct audio_scene_def *scene,
	const struct volume_resolution *vol,
	const struct mixer_stage *stage)
{
	last_applied_scene = *scene;
	last_applied_vol = vol->applied_volume;
	last_applied_pan = scene->pan;
	last_mixer_stage = *stage;
	last_applied_valid = 1;
}

/*
 * Build the differential todo list at machine start: one setter per
 * parameter that differs from the last applied state, in the commit
 * order -- LPF, the changed EQ bands ascending, prefactor, the mixer
 * legs, the output volume/pan. With no recorded state (before any
 * success) everything is written. An empty diff is legitimate: the
 * machine then completes on its first poll with zero DSP writes and
 * the staging it consumed stays consumed (a no-op commit is still a
 * commit).
 */
static void fast_todo_build(void)
{
	struct fast_step *todo = commit.todo;
	const struct audio_scene_def *scene = &commit.scene;
	int all = !last_applied_valid;
	int n = 0;
	int i;

	if (all || scene->lpf_hz != last_applied_scene.lpf_hz) {
		todo[n].op = FAST_LPF;
		todo[n].a = scene->lpf_hz;
		todo[n].b = 0;
		n++;
	}
	for (i = 0; i < AUDIO_SCENE_EQ_BANDS; i++) {
		if (all || scene->eq[i] != last_applied_scene.eq[i]) {
			todo[n].op = FAST_EQ;
			todo[n].a = i;
			todo[n].b = scene->eq[i];
			n++;
		}
	}
	if (all || scene->prefactor != last_applied_scene.prefactor) {
		todo[n].op = FAST_PREF;
		todo[n].a = scene->prefactor;
		todo[n].b = 0;
		n++;
	}
	if (all || commit.stage.paula != last_mixer_stage.paula) {
		todo[n].op = FAST_MIXER_P;
		todo[n].a = commit.stage.paula;
		n++;
	}
	if (all || commit.stage.ax != last_mixer_stage.ax) {
		todo[n].op = FAST_MIXER_A;
		todo[n].a = commit.stage.ax;
		n++;
	}
	if (all || commit.vol.applied_volume != last_applied_vol ||
			scene->pan != last_applied_pan) {
		/* Ramped volume: one intermediate level per service-loop
		 * pass (~0.1 dB per step over Zorro service intervals)
		 * instead of a single step change, so a drag glides. Both
		 * channels move together per level; the target lands last.
		 * A pan-only change or an over-tight todo array falls
		 * back to the direct write. */
		int from = (int)last_applied_vol;
		int to = (int)commit.vol.applied_volume;
		int span = (to > from) ? to - from : from - to;
		int room = (int)(sizeof(commit.todo) / sizeof(commit.todo[0]))
			- n;
		int step;

		if (span > 1 && scene->pan == last_applied_pan &&
				room >= (span - 1) * 2) {
			int dir = (to > from) ? 1 : -1;

			for (step = from + dir; step != to; step += dir) {
				todo[n].op = FAST_VOLPAN_L;
				todo[n].a = (uint8_t)step;
				todo[n].b = scene->pan;
				n++;
				todo[n].op = FAST_VOLPAN_R;
				todo[n].a = (uint8_t)step;
				todo[n].b = scene->pan;
				n++;
			}
		}
		todo[n].op = FAST_VOLPAN_L;
		todo[n].a = commit.vol.applied_volume;
		todo[n].b = scene->pan;
		n++;
		todo[n].op = FAST_VOLPAN_R;
		todo[n].a = commit.vol.applied_volume;
		todo[n].b = scene->pan;
		n++;
	}
	{
		int i;

		for (i = 0; i < n; ++i)
			commit.todo[i].sub = 0;
	}
	commit.todo_count = n;
	commit.todo_next = 0;
}

/*
 * The single commit path (KTD7), as an incremental machine: fade the
 * output down through a verified write, commit the master-chain
 * assignment in fixed order -- LPF, the ten EQ bands as one
 * contiguous safeload group, prefactor, the staged mixer legs -- then
 * restore the resolved output volume. One machine serves scene
 * select, live edit commit, baseline change, and the boot/warm-reset
 * apply (F3); the entry points start it and return without touching
 * I2C, audio_scene_poll() drives the steps (one setter call per
 * call), and a coalesced pending target chains a fresh machine at
 * completion so two rapid scene switches can never tear each other.
 * A live edit (staged commit, direct baseline change) runs the same
 * machine differentially: the changed parameters only, no fade and
 * no restore, so a parameter drag costs one setter call per change
 * instead of the whole fifteen-write sequence.
 */
static void commit_begin(uint8_t index, int fast)
{
	commit.scene_index = index;
	commit.scene = scenes[index];
	commit.eq_band = 0;
	commit.failed = 0;
	commit.fast = fast;
	/* The machine applies a consistent snapshot: mid-flight table
	 * edits belong to the next machine. Both possible reduction
	 * events are emitted here, once per machine, exactly as the
	 * synchronous path emitted them across its run. */
	resolve_output_volume(&commit.scene, &commit.vol);
	if (commit.vol.reduced)
		emit_gain_reduction(commit.vol.requested_level,
			commit.vol.applied_level);
	compute_mixer_stage(commit.vol.chain_linear * commit.vol.linear,
		&commit.stage);
	if (commit.stage.bounded)
		emit_gain_reduction(commit.stage.requested,
			commit.stage.applied);
	commit.owns_rollback = rollback.valid;
	pending_valid = 0;
	pending_target = 0;
	pending_dirty = 0;
	pending_fast = 0;
	if (fast) {
		fast_todo_build();
		commit.phase = commit.todo_count ? COMMIT_FAST :
			COMMIT_DONE;
	} else {
		commit.phase = COMMIT_FADE;
	}
	commit_in_progress = 1;
}

static void commit_finish(void)
{
	commit_in_progress = 0;
	if (commit.failed) {
		/* A mid-sequence failure leaves the chain faded down and
		 * half-written; the abort step has best-effort restored the
		 * resolved output volume so the DAC is not left silent (a
		 * full retry goes through the commit surface). A staged
		 * commit that was optimistically taken from the tables goes
		 * back: the staging survives for the retry. */
		if (rollback.valid) {
			if (rollback.scene_saved) {
				scenes[rollback.scene_index] = rollback.scene;
				staging[rollback.scene_index] = rollback.staging;
			}
			if (rollback.baseline_saved) {
				baseline_paula = rollback.baseline_paula;
				baseline_ax = rollback.baseline_ax;
				baseline_staged = rollback.baseline_staged;
			}
			rollback.valid = 0;
		}
		/* A failed machine drops any coalesced follow-up: the
		 * failure is the terminal outcome and the retry re-enters
		 * through the commit surface. */
		pending_valid = 0;
		pending_dirty = 0;
		return;
	}
	/* Success: the DSP now holds this machine's snapshot. Record it
	 * as the state the next differential commit diffs against -- a
	 * failed machine records nothing, so its retry rewrites the full
	 * diff against the state before the failure. */
	fast_record_applied(&commit.scene, &commit.vol, &commit.stage);
	if (rollback.valid && commit.owns_rollback)
		rollback.valid = 0; /* the staged edits are now applied */
	if (pending_valid &&
		(pending_target != commit.scene_index || pending_dirty)) {
		uint8_t target = pending_target;
		int fast = pending_fast;

		pending_valid = 0;
		pending_dirty = 0;
		pending_fast = 0;
		commit_begin(target, fast);
	}
}

static void commit_step(void)
{
	const struct audio_scene_def *scene = &commit.scene;
	int rc = 0;

	switch (commit.phase) {
	case COMMIT_FADE:
		rc = audio_adau_set_vol_pan(0, scene->pan);
		commit.phase = COMMIT_LPF;
		break;
	case COMMIT_FAST: {
		const struct fast_step *step = &commit.todo[commit.todo_next];

		/* One changed parameter per poll, same as the full
		 * sequence's cadence; the output was never faded, so
		 * there is nothing to restore afterwards. */
		switch (step->op) {
		case FAST_LPF:
			rc = audio_adau_set_lpf_params(step->a);
			break;
		case FAST_EQ: {
			int r = audio_adau_eq_substep(step->a, step->b,
				commit.todo[commit.todo_next].sub++);
			if (r == 0)
				return; /* more substeps: stay on this todo */
			if (r == 1) {
				/* sequence complete: fall to the shared advance */
				rc = 0;
				break;
			}
			rc = -1;
			break;
		}
		case FAST_PREF: {
			int r = audio_adau_safe_prefactor(step->a,
				commit.todo[commit.todo_next].sub++);
			if (r == 0)
				return;
			rc = (r == 1) ? 0 : -1;
			break;
		}
		case FAST_MIXER_P: {
			int r = audio_adau_safe_mixer_leg(0, step->a,
				commit.todo[commit.todo_next].sub++);
			if (r == 0)
				return;
			rc = (r == 1) ? 0 : -1;
			break;
		}
		case FAST_MIXER_A: {
			int r = audio_adau_safe_mixer_leg(1, step->a,
				commit.todo[commit.todo_next].sub++);
			if (r == 0)
				return;
			rc = (r == 1) ? 0 : -1;
			break;
		}
		case FAST_VOLPAN_L: {
			int r = audio_adau_safe_vol_pan_side(0, step->a,
				step->b,
				commit.todo[commit.todo_next].sub++);
			if (r == 0)
				return;
			rc = (r == 1) ? 0 : -1;
			break;
		}
		default: {
			int r = audio_adau_safe_vol_pan_side(1, step->a,
				step->b,
				commit.todo[commit.todo_next].sub++);
			if (r == 0)
				return;
			rc = (r == 1) ? 0 : -1;
			break;
		}
		}
		if (rc == 0 && ++commit.todo_next >= commit.todo_count)
			commit.phase = COMMIT_DONE;
		break;
	}
	case COMMIT_LPF:
		rc = audio_adau_set_lpf_params(scene->lpf_hz);
		commit.eq_band = 0;
		commit.phase = COMMIT_EQ;
		break;
	case COMMIT_EQ:
		rc = audio_adau_set_eq_gain(commit.eq_band,
			scene->eq[commit.eq_band]);
		if (++commit.eq_band >= AUDIO_SCENE_EQ_BANDS)
			commit.phase = COMMIT_PREF;
		break;
	case COMMIT_PREF:
		rc = audio_adau_set_prefactor(scene->prefactor);
		commit.phase = COMMIT_MIXER;
		break;
	case COMMIT_MIXER:
		rc = audio_adau_set_mixer_vol(commit.stage.paula,
			commit.stage.ax);
		commit.phase = COMMIT_RESTORE;
		break;
	case COMMIT_RESTORE:
	case COMMIT_ABORT_RESTORE:
		rc = audio_adau_set_vol_pan(commit.vol.applied_volume,
			scene->pan);
		if (rc != 0) {
			printf("[scene] apply failed; restoring output "
				"volume\n");
			commit.failed = 1;
			if (commit.phase == COMMIT_RESTORE) {
				/* The restore itself failed: retry it once,
				 * best-effort (the sync path did the same),
				 * then finish as a failed commit so a staged
				 * rollback still applies. */
				commit.phase = COMMIT_ABORT_RESTORE;
				return;
			}
			/* The retry failed too: give up, finish failed. */
		}
		commit.phase = COMMIT_DONE;
		commit_finish();
		return;
	default:
		commit.phase = COMMIT_DONE;
		break;
	}
	if (rc != 0 && commit.phase != COMMIT_DONE) {
		printf("[scene] apply failed; restoring output volume\n");
		commit.failed = 1;
		commit.phase = COMMIT_ABORT_RESTORE;
		return;
	}
	if (commit.phase == COMMIT_DONE)
		commit_finish();
}

int audio_scene_poll(void)
{
	if (!commit_in_progress)
		return 0;
	if (commit_stepping)
		return 1; /* nested poll from inside a setter: defer */
	commit_stepping = 1;
	commit_step();
	commit_stepping = 0;
	return commit_in_progress;
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
	memset(staging, 0, sizeof(staging));
	baseline_staged = 0;
	commit_in_progress = 0;
	commit_stepping = 0;
	memset(&commit, 0, sizeof(commit));
	pending_valid = 0;
	pending_target = 0;
	pending_dirty = 0;
	pending_fast = 0;
	memset(&rollback, 0, sizeof(rollback));
	memset(&last_mixer_stage, 0, sizeof(last_mixer_stage));
	/* audio_adau_init leaves the DSP holding the power-on state the
	 * module defaults describe (scene 0 at the default baseline
	 * legs); record it as the applied state so the first
	 * differential commit diffs against the truth. Boot's
	 * apply_after_dsp_init re-establishes it through real writes. */
	{
		struct volume_resolution vol;
		struct mixer_stage stage;

		resolve_output_volume(&scenes[active_scene_index], &vol);
		compute_mixer_stage(vol.chain_linear * vol.linear, &stage);
		fast_record_applied(&scenes[active_scene_index], &vol,
			&stage);
	}
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
	 * the reset tore every owner session -- and every uncommitted
	 * staged edit's client -- down (R10). Any incremental commit in
	 * flight describes the replaced DSP: drop it, along with its
	 * coalesced follow-up and any optimistic staged state. */
	commit_in_progress = 0;
	pending_valid = 0;
	pending_dirty = 0;
	pending_fast = 0;
	rollback.valid = 0;
	/* The DSP was replaced: whatever this module recorded as applied
	 * describes hardware that no longer exists. The full machine
	 * below rewrites everything and re-records on success. */
	last_applied_valid = 0;
	gain_reduction_count = 0;
	have_last_gain_reduction = 0;
	memset(trims, 0, sizeof(trims));
	memset(owner_participating, 0, sizeof(owner_participating));
	memset(staging, 0, sizeof(staging));
	baseline_staged = 0;
	meter_reset_all();
	/* The reset path has no client waiting on the reply, so the
	 * machine runs synchronously to its terminal state here. */
	commit_begin(active_scene_index, 0);
	commit_flush();
	return commit.failed ? -1 : 0;
}

static int scene_def_valid(const struct audio_scene_def *def)
{
	int i;
	int nul = 0;

	if (def->lpf_hz < LPF_HZ_MIN || def->lpf_hz > LPF_HZ_MAX)
		return 0;
	if (def->prefactor > 100 || def->volume > 100 || def->pan > 100)
		return 0;
	for (i = 0; i < AUDIO_SCENE_EQ_BANDS; i++)
		if (def->eq[i] > 100)
			return 0;
	/* The label: printable ASCII up to the first NUL, then a zero
	 * tail (an all-zero name is valid: it means "default label" and
	 * is what a whole-definition write of a freshly zeroed struct
	 * carries). */
	for (i = 0; i <= AUDIO_SCENE_NAME_MAX; i++) {
		char ch = def->name[i];

		if (nul) {
			if (ch != 0)
				return 0;
		} else if (ch == 0) {
			nul = 1;
		} else if (ch < 0x20 || ch > 0x7e) {
			return 0;
		}
	}
	return 1;
}

int audio_scene_select(uint8_t index)
{
	if (index >= AUDIO_SCENE_COUNT || !module_initialized)
		return -1;
	active_scene_index = index;
	if (commit_in_progress) {
		/* Coalesce: the running machine completes first (its
		 * snapshot), then a fresh machine applies this target. */
		pending_valid = 1;
		pending_target = index;
		pending_fast = 0;
		return 0;
	}
	/* A scene select replaces the whole chain: full fade path. */
	commit_begin(index, 0);
	return 0;
}

int audio_scene_write(uint8_t index, const struct audio_scene_def *def)
{
	if (index >= AUDIO_SCENE_COUNT || def == NULL ||
			!module_initialized)
		return -1;
	if (!scene_def_valid(def))
		return -1;
	scenes[index] = *def;
	if (index != active_scene_index)
		return 0;
	if (commit_in_progress) {
		pending_valid = 1;
		pending_target = index;
		pending_dirty = 1;
		pending_fast = 0;
		return 0;
	}
	/* A whole-definition edit replaces the chain: full fade path. */
	commit_begin(index, 0);
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

int audio_scene_set_baseline(uint8_t paula, uint8_t ax)
{
	if (!module_initialized)
		return -1;
	baseline_paula = paula;
	baseline_ax = ax;
	/* The baseline participates in the scene-alone level, so the
	 * change re-runs the one commit path -- differentially: a live
	 * balance change costs only the mixer (and, if the resolution
	 * moved, the output volume) write. */
	if (commit_in_progress) {
		pending_valid = 1;
		pending_target = active_scene_index;
		pending_dirty = 1;
		pending_fast = 1;
		return 0;
	}
	commit_begin(active_scene_index, 1);
	return 0;
}

uint8_t audio_scene_baseline_paula(void)
{
	return baseline_paula;
}

uint8_t audio_scene_baseline_ax(void)
{
	return baseline_ax;
}

/* ---- staged edits and the commit surface (U4) ---- */

int audio_scene_stage_param(uint8_t index, uint32_t param,
	uint32_t value)
{
	struct scene_staging *st;

	if (!module_initialized)
		return -1;
	if (param == SDK_AUDIO_SCENE_PARAM_BASELINE) {
		/* Operator balance (R17): a global, so the scene index is
		 * ignored; it joins the next commit. */
		staged_baseline_paula =
			(uint8_t)SDK_AUDIO_BALANCE_CH1(value);
		staged_baseline_ax = (uint8_t)SDK_AUDIO_BALANCE_CH2(value);
		baseline_staged = 1;
		return 0;
	}
	if (index >= AUDIO_SCENE_COUNT)
		return -1;

	st = &staging[index];
	if (!st->has_draft) {
		/* Seed the draft from the live definition so partial edits
		 * compose; nothing is applied until the commit. A consumed
		 * draft also resets the NAME accumulator: the first NAME
		 * chunk of the next rename opens a fresh name. */
		st->draft = scenes[index];
		st->has_draft = 1;
		st->name_chunks = 0;
		st->name_open = 0;
		st->name_closed = 0;
	}
	if (param == SDK_AUDIO_SCENE_PARAM_NAME) {
		/* Scene label chunk (two printable ASCII chars, or the
		 * 0x0000 terminator). A rename stages the COMPLETE name,
		 * then commits once: the first chunk of a fresh accumulator
		 * clears the old name and lands at chunk 0, terminator
		 * chunks after the first are ignored (zero-padding is
		 * idempotent), and a non-terminator chunk after the
		 * terminator restarts the name -- so a client prepending a
		 * guard terminator self-heals a partial earlier attempt.
		 * Names never join the DSP write set; this only shapes the
		 * label that rides along with the next commit. */
		uint8_t c1 = (uint8_t)((value >> 8) & 0xffU);
		uint8_t c2 = (uint8_t)(value & 0xffU);

		if (value > 0xffffU)
			return -1;
		if (c1 == 0 && c2 == 0) {
			/* Terminator: close the accumulator. On a fresh
			 * draft it opens an empty name (a guard terminator
			 * wipes any residue of an earlier attempt);
			 * terminators after the first are padding no-ops. */
			if (!st->name_open) {
				memset(st->draft.name, 0,
					sizeof(st->draft.name));
				st->name_chunks = 0;
				st->name_open = 1;
			}
			if (!st->name_closed &&
					st->name_chunks <
					AUDIO_SCENE_NAME_CHUNKS) {
				st->draft.name[2 * st->name_chunks] = 0;
				st->draft.name[2 * st->name_chunks + 1] = 0;
			}
			st->name_closed = 1;
			return 0;
		}
		/* Non-terminator: the first char carries the position, so
		 * it must be printable (a NUL first char only exists in
		 * the pure terminator above). */
		if (c1 < 0x20 || c1 > 0x7e)
			return -1;
		if (c2 != 0 && (c2 < 0x20 || c2 > 0x7e))
			return -1;
		if (st->name_closed || !st->name_open) {
			memset(st->draft.name, 0, sizeof(st->draft.name));
			st->name_chunks = 0;
			st->name_open = 1;
			st->name_closed = 0;
		}
		if (st->name_chunks >= AUDIO_SCENE_NAME_CHUNKS)
			return -1; /* 16 chars staged, no room for more */
		st->draft.name[2 * st->name_chunks] = (char)c1;
		st->draft.name[2 * st->name_chunks + 1] = (char)c2;
		st->name_chunks++;
		return 0;
	}
	if (param == SDK_AUDIO_SCENE_PARAM_LPF) {
		if (value < LPF_HZ_MIN || value > LPF_HZ_MAX)
			return -1;
		st->draft.lpf_hz = (uint16_t)value;
		return 0;
	}
	if (param >= SDK_AUDIO_SCENE_PARAM_EQ_BAND_1 &&
			param <= SDK_AUDIO_SCENE_PARAM_EQ_BAND_10) {
		if (value > 100)
			return -1;
		st->draft.eq[param - SDK_AUDIO_SCENE_PARAM_EQ_BAND_1] =
			(uint8_t)value;
		return 0;
	}
	if (param == SDK_AUDIO_SCENE_PARAM_PREFACTOR ||
			param == SDK_AUDIO_SCENE_PARAM_VOLUME ||
			param == SDK_AUDIO_SCENE_PARAM_PAN) {
		if (value > 100)
			return -1;
		if (param == SDK_AUDIO_SCENE_PARAM_PREFACTOR)
			st->draft.prefactor = (uint8_t)value;
		else if (param == SDK_AUDIO_SCENE_PARAM_VOLUME)
			st->draft.volume = (uint8_t)value;
		else
			st->draft.pan = (uint8_t)value;
		return 0;
	}
	return -1;
}

int audio_scene_commit_staged(uint8_t index)
{
	int apply = 0;

	if (!module_initialized || index >= AUDIO_SCENE_COUNT)
		return -1;

	/* The applying machine reads the live tables, so the staged
	 * edits are taken optimistically first; if that machine
	 * ultimately fails, the pre-commit state saved in rollback is
	 * restored and the failed machine does NOT record its state as
	 * applied, so a retry re-writes the diff instead of returning OK
	 * with zero writes (the edit set would be lost). */
	memset(&rollback, 0, sizeof(rollback));
	rollback.scene_index = index;

	/* A staged operator baseline joins the same commit (R17). */
	if (baseline_staged) {
		rollback.baseline_saved = 1;
		rollback.baseline_paula = baseline_paula;
		rollback.baseline_ax = baseline_ax;
		rollback.baseline_staged = baseline_staged;
		baseline_paula = staged_baseline_paula;
		baseline_ax = staged_baseline_ax;
		baseline_staged = 0;
		apply = 1;
	}
	if (staging[index].has_draft) {
		/* Stage-time validation keeps every draft a valid scene. */
		rollback.scene_saved = 1;
		rollback.scene = scenes[index];
		rollback.staging = staging[index];
		scenes[index] = staging[index].draft;
		staging[index].has_draft = 0;
		if (index == active_scene_index)
			apply = 1;
	}
	if (!apply)
		return 0; /* inactive slot, nothing can fail: consumed */

	rollback.valid = 1;
	if (commit_in_progress) {
		/* A machine is mid-flight on its own snapshot; the staged
		 * state is applied by the coalesced follow-up machine. */
		pending_valid = 1;
		pending_target = active_scene_index;
		pending_dirty = 1;
		pending_fast = 1;
		return 0;
	}
	/* A live edit commit runs differentially: only the parameters
	 * the staging changed, no fade envelope. */
	commit_begin(active_scene_index, 1);
	return 0;
}

void audio_scene_control_state(struct audio_scene_control_state *out)
{
	if (out == NULL)
		return;
	out->active_scene = active_scene_index;
	out->scene_count = AUDIO_SCENE_COUNT;
	out->baseline_paula = baseline_paula;
	out->baseline_ax = baseline_ax;
	out->trim_paula = last_mixer_stage.paula;
	out->trim_ax = last_mixer_stage.ax;
	out->trim_bounded = last_mixer_stage.bounded;
	out->ceiling = (uint32_t)AUDIO_SCENE_ENFORCED_BOUNDARY;
}

static int appendf(char *buf, unsigned size, int off, const char *fmt, ...)
{
	va_list ap;
	int n;

	if (off < 0 || (unsigned)off >= size) return -1;
	va_start(ap, fmt);
	n = vsnprintf(buf + off, size - off, fmt, ap);
	va_end(ap);
	if (n < 0 || (unsigned)n >= size - off) return -1;
	return off + n;
}

/* Serialize the live audio state as the CFG audio block (KTD4 key
 * grammar; zz_config.c parses the same names). Appends at off and
 * returns the new length, or -1 when it does not fit. */
static int audio_scene_emit_keys(char *buf, unsigned size, int off)
{
	int i, k;

	off = appendf(buf, size, off, "audio_active = %u\n",
		(unsigned)active_scene_index);
	if (off < 0) return -1;
	off = appendf(buf, size, off, "audio_baseline = %u\n",
		((unsigned)baseline_paula << 8) | (unsigned)baseline_ax);
	if (off < 0) return -1;

	for (i = 0; i < AUDIO_SCENE_COUNT; i++) {
		const struct audio_scene_def *s = &scenes[i];

		off = appendf(buf, size, off, "audio_scene%d_lpf = %u\n",
			i, (unsigned)s->lpf_hz);
		if (off < 0) return -1;
		for (k = 0; k < 5; k++) {
			off = appendf(buf, size, off,
				"audio_scene%d_eq%d%d = %u\n",
				i, 2 * k, 2 * k + 1,
				(unsigned)s->eq[2 * k] * 128u +
				(unsigned)s->eq[2 * k + 1]);
			if (off < 0) return -1;
		}
		off = appendf(buf, size, off, "audio_scene%d_out = %u\n",
			i, (unsigned)s->prefactor * 128u +
			(unsigned)s->volume);
		if (off < 0) return -1;
		off = appendf(buf, size, off, "audio_scene%d_pan = %u\n",
			i, (unsigned)s->pan);
		if (off < 0) return -1;
		/* The label, two chars per key, zero-padded with
		 * terminator chunks (nm1..nm8 share the CFG grammar with
		 * the SCENE_WRITE NAME param). */
		for (k = 0; k < AUDIO_SCENE_NAME_CHUNKS; k++) {
			off = appendf(buf, size, off,
				"audio_scene%d_nm%d = %u\n",
				i, k + 1,
				(unsigned)(uint8_t)s->name[2 * k] * 256u +
				(unsigned)(uint8_t)s->name[2 * k + 1]);
			if (off < 0) return -1;
		}
	}
	return off;
}


int audio_scene_save(uint8_t index)
{
	struct volume_resolution vol;
	int i;

	if (index >= AUDIO_SCENE_COUNT || !module_initialized)
		return -1;
	/* Flush any running incremental commit first (bounded drain): a
	 * staged commit that will fail restores the pre-commit tables at
	 * its abort, and validation must see that settled state rather
	 * than the optimistic mid-flight one. */
	commit_flush();
	/* R15: validate against the enforced boundary first -- a scene
	 * whose own master-chain level exceeds it never reaches the
	 * writer. Same evaluation the apply path stages with. The writer
	 * below persists every slot, so every slot is validated with the
	 * current baseline: one over-boundary scene anywhere rejects the
	 * whole save. */
	for (i = 0; i < AUDIO_SCENE_COUNT; i++) {
		resolve_output_volume(&scenes[i], &vol);
		if (vol.reduced)
			return AUDIO_SCENE_SAVE_REJECTED;
	}
	return audio_scene_cfg_write();
}

int audio_scene_cfg_write(void)
{
	static char buf[ZZ_CONFIG_MAX_SIZE];
	int n;

	if (!module_initialized)
		return AUDIO_SCENE_SAVE_IO_ERROR;

	/* U5 content policy: regenerate ZZ9000.CFG from parsed state
	 * (every present known key, comments not preserved) plus the live
	 * audio state -- the parsed audio keys are never echoed back
	 * stale. */
	n = snprintf(buf, sizeof(buf),
		"# " ZZ_CONFIG_FILENAME " written by the firmware audio "
		"Save; comments are not preserved\n");
	if (n < 0 || (unsigned)n >= sizeof(buf))
		return AUDIO_SCENE_SAVE_IO_ERROR;
	n = zz_config_emit_present_keys(buf, sizeof(buf), n);
	if (n < 0)
		return AUDIO_SCENE_SAVE_IO_ERROR;
	n = audio_scene_emit_keys(buf, sizeof(buf), n);
	if (n < 0)
		return AUDIO_SCENE_SAVE_IO_ERROR;
	if (zz_config_save_file(buf, (unsigned)n) != 0)
		return AUDIO_SCENE_SAVE_IO_ERROR;
	return AUDIO_SCENE_SAVE_OK;
}

void audio_scene_load_config(void)
{
	const struct zz_config *c = zz_config_get();
	int i, k;

	/* Fold the parsed audio keys into scene state (R10). Absent keys
	 * keep the built-in defaults; a stored scene that no longer
	 * validates degrades to its default as a unit. */
	for (i = 0; i < AUDIO_SCENE_COUNT; i++) {
		struct audio_scene_def def = default_scenes[i];
		uint16_t mask = c->audio_scene_mask[i];
		uint16_t packed;

		if (mask & (1u << 0))
			def.lpf_hz = c->audio_scene_lpf[i];
		for (k = 0; k < 5; k++) {
			if (!(mask & (1u << (1 + k))))
				continue;
			packed = c->audio_scene_eq[i][k];
			def.eq[2 * k] = (uint8_t)(packed / 128);
			def.eq[2 * k + 1] = (uint8_t)(packed % 128);
		}
		if (mask & (1u << 6)) {
			packed = c->audio_scene_out[i];
			def.prefactor = (uint8_t)(packed / 128);
			def.volume = (uint8_t)(packed % 128);
		}
		if (mask & (1u << 7))
			def.pan = (uint8_t)c->audio_scene_pan[i];
		/* The label chunks rebuild the name as a unit: absent
		 * chunks are the zero padding the writer emits, and an
		 * all-NUL result (an explicitly emptied name) falls back
		 * to the built-in default label. */
		if (mask & 0xff00u) {
			memset(def.name, 0, sizeof(def.name));
			for (k = 0; k < AUDIO_SCENE_NAME_CHUNKS; k++) {
				if (!(mask & (1u << (8 + k))))
					continue;
				packed = c->audio_scene_nm[i][k];
				def.name[2 * k] = (char)(uint8_t)(packed >> 8);
				def.name[2 * k + 1] =
					(char)(uint8_t)(packed & 0xff);
			}
			if (def.name[0] == 0)
				strcpy(def.name, default_scenes[i].name);
		}
		if (mask && scene_def_valid(&def))
			scenes[i] = def;
	}
	if (c->audio_active_present &&
	    c->audio_active < AUDIO_SCENE_COUNT)
		active_scene_index = (uint8_t)c->audio_active;
	if (c->audio_baseline_present) {
		baseline_paula = (uint8_t)(c->audio_baseline >> 8);
		baseline_ax = (uint8_t)(c->audio_baseline & 0xff);
	}
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
	/* A machine mid-flight would overwrite these legs with its
	 * start-time snapshot at its mixer step; coalesce a re-apply so
	 * the trim lands for good. The submit's own write above still
	 * satisfies the immediate applied-legs report. */
	if (commit_in_progress) {
		pending_valid = 1;
		pending_target = active_scene_index;
		pending_dirty = 1;
	}

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
	if (commit_in_progress) {
		pending_valid = 1;
		pending_target = active_scene_index;
		pending_dirty = 1;
	}
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
