/*
 * Host tests for the audio control-plane mailbox dispatch (plan U4):
 * the six SDK_OP_AUDIO_* opcodes (0x0509..0x050e) against the scene
 * module, including the KTD7 fade-commit-restore sequence.
 *
 * The dispatch layer (sdk_audio_control.c) is the exact function the
 * core-0 mailbox handler in sdk_mailbox.c routes these opcodes to, so
 * every assertion here exercises firmware behavior, not a copy: each
 * opcode round-trips (select then CONTROL_STATE_GET reflects it),
 * staged SCENE_WRITE accumulates firmware-side and only COMMIT
 * applies, trim submit reports applied/bound/bounded per the ABI,
 * commit ordering is asserted through the recording setter stub (fade
 * writes precede param writes precede restore; the EQ bands commit as
 * one contiguous safeload group), two rapid scene switches serialize
 * with no interleaved partial commits (including a nested select from
 * inside the commit itself, which coalesces rather than interleaving),
 * meter reads return framed
 * snapshots, SCENE_SAVE validates synchronously then starts the
 * non-blocking save machine (QUEUED; BUSY while one runs; the
 * settled outcome rides the control-state report's save_status
 * against the linked FatFs mock), and unknown
 * opcodes still complete SDK_STATUS_UNSUPPORTED. Scene commits apply
 * through the incremental machine (P1): dispatch opcodes return
 * before any I2C and the tests drain the machine with
 * audio_scene_poll before asserting on the write log.
 *
 * Linked like audio_scene_test: the ax.h DSP setters are recording
 * stubs (link-time seam), so every master-chain write is observable
 * in order.
 */

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "audio_scene.h"
#include "ax.h"
#include "ff.h"
#include "sdk_audio_control.h"
#include "sdk_mailbox.h"

/* ---- recording stubs for the ax.h DSP setters ---- */

#include "dsp_write_mock.h"


/* Big-endian word accessors: pack requests and read results exactly
 * like an SDK client (abi.h accessors) would, proving the firmware
 * byte order on both sides of the dispatch. */
static void put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

static uint32_t w32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* Nested dispatch helper used by the EQ stub below. */
static uint16_t run_local_select(
	const struct SDKAudioSceneSelectPayload *payload);

/* ---- commit-machine drain (the firmware main loop polls) ---- */

/* The scene module applies commits through the incremental machine:
 * dispatch opcodes start it and return before any I2C, the main loop
 * advances it one setter call per pass. Tests drain it synchronously
 * to its terminal state before asserting on the write log. */
static void pump_scene(void)
{
	int guard = 0;

	while (audio_scene_poll() && ++guard < 1000)
		;
}

/* ---- recording stubs for the ax.h DSP setters ---- */


/*
 * Reentrancy hook: re-enter the dispatch from inside the commit's EQ
 * group. The second select arrives while a machine is mid-flight: it
 * must be coalesced (SDK_STATUS_OK, no writes of its own) and applied
 * by a fresh machine once the outer one completes.
 */
static int reenter_armed;
static uint16_t nested_status = 0xffffU;
static int nested_write_count = -1;

int audio_adau_set_lpf_params(int f0)
{
	record_write(WRITE_LPF, f0, 0);
	return 0;
}

int audio_adau_set_mixer_vol(int vol1, int vol2)
{
	record_write(WRITE_MIXER, vol1, vol2);
	return 0;
}

int audio_adau_set_prefactor(int pre)
{
	record_write(WRITE_PREF, pre, 0);
	return 0;
}

int audio_adau_set_eq_gain(int band, int gain)
{
	record_write(WRITE_EQ, band, gain);
	if (reenter_armed && band == 5) {
		struct SDKAudioSceneSelectPayload nested;

		memset(&nested, 0, sizeof(nested));
		put32(nested.scene, 4);
		reenter_armed = 0;
		nested_write_count = write_count;
		nested_status = run_local_select(&nested);
	}
	return 0;
}

int audio_adau_set_vol_pan(int vol, int pan)
{
	record_write(WRITE_VOLPAN, vol, pan);
	return 0;
}

int audio_adau_set_vol_pan_side(int side, int vol, int pan)
{
	record_write(WRITE_VOLPAN_SIDE0 + side, vol, pan);
	return 0;
}

int audio_adau_eq_substep(int band, int gain, int substep)
{
	(void)gain; record_write(WRITE_EQ_SUB, band, substep);
	if (substep < 10) return 0;
	return 1;
}
__attribute__((unused)) int audio_adau_safe_mixer_leg(int leg, int value,
	int substep)
{
	record_write(WRITE_MIXER_P + leg, value, 0);
	if (substep == 2)
		return audio_adau_safeload_latch_result(
			fail_next_write ? -1 : 0);
	return 0;
}

__attribute__((unused)) int audio_adau_safe_vol_pan_side(int side, int vol,
	int pan, int substep)
{
	record_write(WRITE_VOLPAN_SIDE0 + side, vol, pan);
	if (substep == 2)
		return audio_adau_safeload_latch_result(
			fail_next_write ? -1 : 0);
	return 0;
}

__attribute__((unused)) int audio_adau_safe_prefactor(int pre, int substep)
{
	record_write(WRITE_PREF, pre, 0);
	return (substep < 4) ? 0 :
		audio_adau_safeload_latch_result(0);
}


int audio_adau_set_mixer_leg(int leg, int value)
{
	record_write(WRITE_MIXER_P + leg, value, 0);
	return 0;
}

/* ---- assertions (suite convention) ---- */

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

static uint8_t result_buf[48];
static uint16_t result_len;

static uint16_t run_op(uint16_t opcode, const void *payload,
	uint16_t payload_len)
{
	memset(result_buf, 0, sizeof(result_buf));
	result_len = 0;
	return sdk_audio_control_run(opcode, payload, payload_len,
		result_buf, &result_len);
}

static uint16_t run_local_select(
	const struct SDKAudioSceneSelectPayload *payload)
{
	return run_op(SDK_OP_AUDIO_SCENE_SELECT, payload, sizeof(*payload));
}

/* ---- one full KTD7 commit sequence, asserted entry by entry ---- */

/*
 * Expected log layout for one commit (fade -> ordered params ->
 * restore), starting at log index base:
 *   base+0     VOLPAN(0, pan)        fade down (verified write)
 *   base+1     LPF(lpf)
 *   base+2..11 EQ(band 0..9, gain)   one contiguous safeload group
 *   base+12    PREF(pref)
 *   base+13    MIXER(m1, m2)         staged mixer legs
 *   base+14    VOLPAN(vol, pan)      restore
 */
static void check_commit_sequence(const char *name, int base, int lpf,
	const uint8_t *eq, int pref, int vol, int pan, int m1, int m2)
{
	int i;
	int kind = 0, a = 0, b = 0;
	int ok;

	ok = log_at(base, &kind, &a, &b) && kind == WRITE_VOLPAN &&
		a == 0 && b == pan;
	check(ok, "commit fades output down first",
		fmt("%s[%d]: kind=%d vol=%d pan=%d", name, base, kind, a, b));
	ok = log_at(base + 1, &kind, &a, &b) && kind == WRITE_LPF &&
		a == lpf;
	check(ok, "commit writes LPF",
		fmt("%s[%d]: kind=%d f0=%d", name, base + 1, kind, a));
	for (i = 0; i < AUDIO_SCENE_EQ_BANDS; i++) {
		ok = log_at(base + 2 + i, &kind, &a, &b) &&
			kind == WRITE_EQ && a == i && b == eq[i];
		check(ok, "commit writes EQ bands as one contiguous group",
			fmt("%s[%d]: kind=%d band=%d gain=%d want=%u",
				name, base + 2 + i, kind, a, b, eq[i]));
	}
	ok = log_at(base + 12, &kind, &a, &b) && kind == WRITE_PREF &&
		a == pref;
	check(ok, "commit writes prefactor",
		fmt("%s[%d]: kind=%d pre=%d", name, base + 12, kind, a));
	ok = log_at(base + 13, &kind, &a, &b) && kind == WRITE_MIXER &&
		a == m1 && b == m2;
	check(ok, "commit stages mixer legs",
		fmt("%s[%d]: kind=%d v1=%d v2=%d", name, base + 13, kind,
			a, b));
	ok = log_at(base + 14, &kind, &a, &b) && kind == WRITE_VOLPAN &&
		a == vol && b == pan;
	check(ok, "commit restores output volume last",
		fmt("%s[%d]: kind=%d vol=%d pan=%d", name, base + 14, kind,
			a, b));
}

static const uint8_t eq_unity[AUDIO_SCENE_EQ_BANDS] = {
	50, 50, 50, 50, 50, 50, 50, 50, 50, 50
};

/* ---- SCENE_SELECT / CONTROL_STATE_GET round-trip ---- */

static void test_select_state_roundtrip(void)
{
	struct SDKAudioSceneSelectPayload sel;
	struct SDKAudioControlStateGetPayload get;
	uint16_t status;
	int a = -1, b = -1;

	audio_scene_init();
	memset(&sel, 0, sizeof(sel));
	memset(&get, 0, sizeof(get));

	put32(sel.scene, 3);
	status = run_op(SDK_OP_AUDIO_SCENE_SELECT, &sel, sizeof(sel));
	check(status == SDK_STATUS_OK && result_len == 0,
		"scene select dispatch completes OK",
		fmt("status=%u len=%u", status, result_len));
	pump_scene();

	status = run_op(SDK_OP_AUDIO_CONTROL_STATE_GET, &get, sizeof(get));
	check(status == SDK_STATUS_OK && result_len == sizeof(
			struct SDKAudioControlStateResultPayload),
		"control state get completes with result payload",
		fmt("status=%u len=%u", status, result_len));
	check(w32(&result_buf[0]) == 3 && w32(&result_buf[4]) ==
		SDK_AUDIO_SCENE_COUNT,
		"state get reflects the selected scene",
		fmt("active=%lu count=%lu",
			(unsigned long)w32(&result_buf[0]),
			(unsigned long)w32(&result_buf[4])));
	check(w32(&result_buf[24]) ==
			AUDIO_SCENE_DEFAULT_CEILING_PAULA &&
		w32(&result_buf[28]) == AUDIO_SCENE_DEFAULT_CEILING_AX,
		"state get reports default per-leg ceilings", NULL);
	check(audio_scene_active_index() == 3,
		"module active index agrees", NULL);
	check(last_write(WRITE_VOLPAN, &a, &b) && a == 75 && b == 50,
		"default scene 3 volume restored",
		fmt("vol=%d pan=%d", a, b));
}

/* ---- staged SCENE_WRITE: accumulates, only COMMIT applies ---- */

static void test_staged_write_accumulates(void)
{
	struct SDKAudioSceneWritePayload wr;
	uint16_t status;
	int kind = 0, a = 0, b = 0;
	int ok;

	audio_scene_init();
	clear_writes();
	memset(&wr, 0, sizeof(wr));

	/* Stage two parameters without COMMIT: nothing reaches the DSP
	 * and the stored scene definition is untouched. */
	put32(wr.scene, 0);
	put32(wr.param, SDK_AUDIO_SCENE_PARAM_EQ_BAND_4);
	put32(wr.value, 30);
	status = run_op(SDK_OP_AUDIO_SCENE_WRITE, &wr, sizeof(wr));
	check(status == SDK_STATUS_OK,
		"staged EQ write accepted without commit",
		fmt("status=%u", status));
	put32(wr.param, SDK_AUDIO_SCENE_PARAM_VOLUME);
	put32(wr.value, 60);
	status = run_op(SDK_OP_AUDIO_SCENE_WRITE, &wr, sizeof(wr));
	check(status == SDK_STATUS_OK,
		"staged volume write accepted without commit",
		fmt("status=%u", status));
	check(write_count == 0,
		"staging alone issues no DSP writes",
		fmt("writes=%d", write_count));
	check(audio_scene_get(0) != NULL &&
		audio_scene_get(0)->eq[3] == 50 &&
		audio_scene_get(0)->volume == 100,
		"staged values not applied to the scene definition", NULL);

	/* An invalid stage value is refused and leaves staging intact. */
	put32(wr.param, SDK_AUDIO_SCENE_PARAM_EQ_BAND_4);
	put32(wr.value, 101);
	status = run_op(SDK_OP_AUDIO_SCENE_WRITE, &wr, sizeof(wr));
	check(status == SDK_STATUS_BAD_REQUEST,
		"out-of-range EQ gain rejected at stage time",
		fmt("status=%u", status));
	put32(wr.param, 0);
	put32(wr.value, 1);
	status = run_op(SDK_OP_AUDIO_SCENE_WRITE, &wr, sizeof(wr));
	check(status == SDK_STATUS_BAD_REQUEST,
		"unknown param id rejected",
		fmt("status=%u", status));
	check(write_count == 0,
		"rejected staging issues no DSP writes",
		fmt("writes=%d", write_count));

	/* COMMIT applies everything staged so far through the
	 * differential path: only the changed parameters, in commit
	 * order, with no fade and no restore. */
	put32(wr.param, SDK_AUDIO_SCENE_PARAM_VOLUME);
	put32(wr.value, 60);
	put32(wr.flags, SDK_AUDIO_SCENE_WRITE_FLAG_COMMIT);
	status = run_op(SDK_OP_AUDIO_SCENE_WRITE, &wr, sizeof(wr));
	check(status == SDK_STATUS_OK,
		"commit applies the staged group",
		fmt("status=%u", status));
	check(write_count == 0,
		"commit dispatch issued no DSP writes before poll",
		fmt("writes=%d", write_count));
	pump_scene();
	check(write_count == 251,
		"staged commit: 11 EQ + 40-level ramp x 2 x 3 substeps",
		fmt("writes=%d", write_count));
	ok = log_at(0, &kind, &a, &b) && kind == WRITE_EQ_SUB &&
		a == 3 && b == 0;
	check(ok, "staged commit starts the EQ substep run",
		fmt("kind=%d band=%d sub=%d", kind, a, b));
	ok = last_write(WRITE_VOLPAN_SIDE1, &a, &b) && a == 60 && b == 50;
	check(ok, "staged commit ramp lands the target (R)",
		fmt("kind=%d vol=%d pan=%d", kind, a, b));
	check(audio_scene_get(0) != NULL &&
		audio_scene_get(0)->eq[3] == 30 &&
		audio_scene_get(0)->volume == 60,
		"committed values stored in the scene definition", NULL);
	check(audio_scene_gain_reduction_events() == 0,
		"within-boundary staged commit emits no event", NULL);
}

/* ---- KTD7 commit ordering via the default scenes ---- */

static void test_commit_ordering(void)
{
	struct SDKAudioSceneSelectPayload sel;
	static const uint8_t eq3[AUDIO_SCENE_EQ_BANDS] = {
		50, 50, 50, 50, 50, 50, 55, 55, 50, 50
	};

	audio_scene_init();
	clear_writes();
	memset(&sel, 0, sizeof(sel));

	put32(sel.scene, 3);
	check(run_op(SDK_OP_AUDIO_SCENE_SELECT, &sel, sizeof(sel)) ==
		SDK_STATUS_OK, "select scene 3", NULL);
	pump_scene();
	check(write_count == 15,
		"one commit sequence per select",
		fmt("writes=%d", write_count));
	check_commit_sequence("scene 3", 0, 18000, eq3, 50, 75, 50, 128,
		64);
}

/* ---- two rapid switches serialize ---- */

static void test_rapid_switches_serialize(void)
{
	struct SDKAudioSceneSelectPayload sel;
	static const uint8_t eq2[AUDIO_SCENE_EQ_BANDS] = {
		55, 55, 50, 50, 50, 50, 50, 50, 50, 50
	};
	static const uint8_t eq5[AUDIO_SCENE_EQ_BANDS] = {
		45, 45, 50, 50, 50, 50, 50, 50, 45, 45
	};

	audio_scene_init();
	clear_writes();
	memset(&sel, 0, sizeof(sel));

	put32(sel.scene, 2);
	check(run_op(SDK_OP_AUDIO_SCENE_SELECT, &sel, sizeof(sel)) ==
		SDK_STATUS_OK, "rapid switch A", NULL);
	put32(sel.scene, 5);
	check(run_op(SDK_OP_AUDIO_SCENE_SELECT, &sel, sizeof(sel)) ==
		SDK_STATUS_OK, "rapid switch B", NULL);
	/* Both dispatches returned without I2C; draining the machine
	 * applies A completely first, then B (coalesced onto A's
	 * completion): two full sequences, A's restore (index 14)
	 * before B's fade (index 15). */
	pump_scene();

	check(write_count == 30,
		"two rapid switches produce two complete sequences",
		fmt("writes=%d", write_count));
	check_commit_sequence("switch A", 0, 16000, eq2, 50, 75, 50, 128,
		64);
	check_commit_sequence("switch B", 15, 12000, eq5, 50, 90, 50, 128,
		64);
}

/* ---- nested select during a commit coalesces, never interleaves ---- */

static void test_nested_commit_coalesces(void)
{
	struct SDKAudioSceneSelectPayload sel;

	audio_scene_init();
	clear_writes();
	memset(&sel, 0, sizeof(sel));
	nested_status = 0xffffU;
	nested_write_count = -1;
	reenter_armed = 1;
	put32(sel.scene, 3);
	check(run_op(SDK_OP_AUDIO_SCENE_SELECT, &sel, sizeof(sel)) ==
		SDK_STATUS_OK, "outer dispatch accepted", NULL);
	check(write_count == 0,
		"outer dispatch issued no DSP writes before poll",
		fmt("writes=%d", write_count));
	/* The reentry hook fires inside the machine's EQ group (only
	 * poll issues writes now): the nested select must coalesce. */
	pump_scene();
	check(nested_status == SDK_STATUS_OK,
		"select re-entered mid-machine is coalesced, not BUSY",
		fmt("status=%u", nested_status));
	check(nested_write_count == 8,
		"nested attempt happened inside the EQ group",
		fmt("writes at nested call=%d", nested_write_count));
	check(write_count == 30,
		"outer machine completes, then the coalesced machine applies",
		fmt("writes=%d", write_count));
	check_commit_sequence("outer", 0, 18000,
		(const uint8_t[]){ 50, 50, 50, 50, 50, 50, 55, 55, 50, 50 },
		50, 75, 50, 128, 64);
	check_commit_sequence("coalesced", 15, 23900, eq_unity, 60, 70,
		50, 128, 64);
	check(audio_scene_active_index() == 4,
		"coalesced selection becomes the active scene", NULL);
}

/* ---- trim submit: applied / bound / bounded flag ---- */

static void test_trim_submit_result(void)
{
	struct SDKAudioTrimSubmitPayload tr;
	struct SDKAudioControlStateGetPayload get;
	uint32_t flags, applied, bound, baseline, trim;
	uint16_t status;

	audio_scene_init();
	memset(&tr, 0, sizeof(tr));
	memset(&get, 0, sizeof(get));

	/* Scene 0 (unity) with the default baseline (128+64 = 192, at
	 * the boundary): an absolute balance below the composed limit is
	 * applied verbatim, unbounded. */
	put32(tr.balance, SDK_AUDIO_BALANCE_PACK(108, 44));
	status = run_op(SDK_OP_AUDIO_TRIM_SUBMIT, &tr, sizeof(tr));
	check(status == SDK_STATUS_OK && result_len == sizeof(
			struct SDKAudioTrimResultPayload),
		"trim submit completes with result payload",
		fmt("status=%u len=%u", status, result_len));
	applied = w32(&result_buf[0]);
	bound = w32(&result_buf[4]);
	flags = w32(&result_buf[8]);
	check(applied == SDK_AUDIO_BALANCE_PACK(108, 44) &&
		flags == 0 && bound == 0,
		"within-boundary trim applied verbatim, unbounded",
		fmt("applied=0x%lx bound=0x%lx flags=%lu",
			(unsigned long)applied, (unsigned long)bound,
			(unsigned long)flags));

	/* One step over the composed boundary: reduced legs, BOUNDED
	 * flag, and the applied bound reported back (138+64 = 202 scaled
	 * by 192/202 -> 131+60 = 191). */
	put32(tr.balance, SDK_AUDIO_BALANCE_PACK(138, 64));
	status = run_op(SDK_OP_AUDIO_TRIM_SUBMIT, &tr, sizeof(tr));
	check(status == SDK_STATUS_OK, "bounded trim submit accepted",
		fmt("status=%u", status));
	applied = w32(&result_buf[0]);
	bound = w32(&result_buf[4]);
	flags = w32(&result_buf[8]);
	check(applied == SDK_AUDIO_BALANCE_PACK(131, 60) &&
		flags == SDK_AUDIO_TRIM_RESULT_BOUNDED &&
		bound == SDK_AUDIO_BALANCE_PACK(131, 60),
		"over-boundary trim bounded and reported",
		fmt("applied=0x%lx bound=0x%lx flags=%lu",
			(unsigned long)applied, (unsigned long)bound,
			(unsigned long)flags));
	check(audio_scene_gain_reduction_events() == 1,
		"bounded trim emitted one gain-reduction event", NULL);

	/* CONTROL_STATE_GET mirrors the applied trim and the bounded
	 * flag, and reports the baseline and ceiling. */
	status = run_op(SDK_OP_AUDIO_CONTROL_STATE_GET, &get, sizeof(get));
	check(status == SDK_STATUS_OK, "state get after trim", NULL);
	flags = w32(&result_buf[20]);
	baseline = w32(&result_buf[8]);
	trim = w32(&result_buf[12]);
	check(flags == SDK_AUDIO_CONTROL_FLAG_TRIM_BOUNDED &&
		trim == SDK_AUDIO_BALANCE_PACK(131, 60),
		"state get reports bounded applied trim",
		fmt("flags=0x%lx trim=0x%lx", (unsigned long)flags,
			(unsigned long)trim));
	check(baseline == SDK_AUDIO_BALANCE_PACK(128, 64) &&
		w32(&result_buf[16]) == 192,
		"state get reports baseline pair and enforced ceiling",
		fmt("baseline=0x%lx ceiling=%lu",
			(unsigned long)baseline,
			(unsigned long)w32(&result_buf[16])));
}

/* ---- trim submit: the reserved neutral word keeps the baseline ---- */

static void test_trim_neutral_keep_baseline(void)
{
	struct SDKAudioTrimSubmitPayload tr;
	struct SDKAudioSceneWritePayload wr;
	uint32_t flags, applied, bound;
	uint16_t status;

	audio_scene_init();
	memset(&tr, 0, sizeof(tr));
	memset(&wr, 0, sizeof(wr));

	/* A non-default operator baseline (150/40) through the staged
	 * commit path, so the default 128/64 pair cannot mask the
	 * distinction between "baseline" and "neutral word". */
	put32(wr.scene, 0);
	put32(wr.param, SDK_AUDIO_SCENE_PARAM_BASELINE);
	put32(wr.value, SDK_AUDIO_BALANCE_PACK(150, 40));
	put32(wr.flags, SDK_AUDIO_SCENE_WRITE_FLAG_COMMIT);
	check(run_op(SDK_OP_AUDIO_SCENE_WRITE, &wr, sizeof(wr)) ==
		SDK_STATUS_OK, "non-default baseline committed", NULL);
	pump_scene();

	/* The reserved 0x7f7f word is keep-baseline: "no trim from this
	 * owner". The reply reports the baseline pair as applied,
	 * unbounded, and the mixer is not restaged -- no DSP write and no
	 * gain-reduction event. */
	clear_writes();
	put32(tr.balance, SDK_AUDIO_BALANCE_NEUTRAL);
	status = run_op(SDK_OP_AUDIO_TRIM_SUBMIT, &tr, sizeof(tr));
	check(status == SDK_STATUS_OK && result_len == sizeof(
			struct SDKAudioTrimResultPayload),
		"neutral trim submit completes with result payload",
		fmt("status=%u len=%u", status, result_len));
	applied = w32(&result_buf[0]);
	bound = w32(&result_buf[4]);
	flags = w32(&result_buf[8]);
	check(applied == SDK_AUDIO_BALANCE_PACK(150, 40) &&
		flags == 0 && bound == 0,
		"neutral submit reports the baseline pair, unbounded",
		fmt("applied=0x%lx bound=0x%lx flags=%lu",
			(unsigned long)applied, (unsigned long)bound,
			(unsigned long)flags));
	check(write_count == 0,
		"neutral submit does not restage the mixer",
		fmt("writes=%d", write_count));
	check(audio_scene_gain_reduction_events() == 0,
		"neutral submit emits no gain-reduction event", NULL);

	/* Absolute requests still convert against the baseline: asking
	 * for 140/30 lands exactly that pair (baseline-relative deltas,
	 * composed within the boundary), proving the reserved word did
	 * not redefine the ordinary path. */
	put32(tr.balance, SDK_AUDIO_BALANCE_PACK(140, 30));
	status = run_op(SDK_OP_AUDIO_TRIM_SUBMIT, &tr, sizeof(tr));
	check(status == SDK_STATUS_OK, "absolute trim submit accepted",
		fmt("status=%u", status));
	applied = w32(&result_buf[0]);
	bound = w32(&result_buf[4]);
	flags = w32(&result_buf[8]);
	check(applied == SDK_AUDIO_BALANCE_PACK(140, 30) &&
		flags == 0 && bound == 0,
		"absolute submit still converts against the baseline",
		fmt("applied=0x%lx bound=0x%lx flags=%lu",
			(unsigned long)applied, (unsigned long)bound,
			(unsigned long)flags));
}


/* ---- trim submit: the neutral word releases a held SDK trim ---- */

/*
 * Review 3854408614: after a non-neutral submission took the SDK
 * trim slot, the reserved neutral word is the documented release
 * path. It must drop that trim before reporting the operator
 * baseline, so the applied state and the DSP both return to the
 * baseline pair -- and, with the slot already released, a repeat
 * neutral submit stays write-free (release is idempotent).
 */
static void test_trim_neutral_releases_held_trim(void)
{
	struct SDKAudioTrimSubmitPayload tr;
	struct SDKAudioControlStateGetPayload get;
	uint32_t flags, applied, bound;
	uint16_t status;
	int a = -1, b = -1;

	audio_scene_init();
	memset(&tr, 0, sizeof(tr));
	memset(&get, 0, sizeof(get));

	/* A non-neutral balance takes the SDK trim slot and moves the
	 * applied mixer legs off the baseline. */
	put32(tr.balance, SDK_AUDIO_BALANCE_PACK(108, 44));
	check(run_op(SDK_OP_AUDIO_TRIM_SUBMIT, &tr, sizeof(tr)) ==
		SDK_STATUS_OK, "non-neutral trim accepted", NULL);
	applied = w32(&result_buf[0]);
	check(applied == SDK_AUDIO_BALANCE_PACK(108, 44),
		"held trim reports its applied pair",
		fmt("applied=0x%lx", (unsigned long)applied));

	/* The neutral word releases: the baseline pair is reported and
	 * actually applied. */
	clear_writes();
	put32(tr.balance, SDK_AUDIO_BALANCE_NEUTRAL);
	status = run_op(SDK_OP_AUDIO_TRIM_SUBMIT, &tr, sizeof(tr));
	check(status == SDK_STATUS_OK, "neutral release accepted",
		fmt("status=%u", status));
	applied = w32(&result_buf[0]);
	bound = w32(&result_buf[4]);
	flags = w32(&result_buf[8]);
	check(applied == SDK_AUDIO_BALANCE_PACK(128, 64) &&
		flags == 0 && bound == 0,
		"neutral release reports the baseline pair, unbounded",
		fmt("applied=0x%lx bound=0x%lx flags=0x%lx",
			(unsigned long)applied, (unsigned long)bound,
			(unsigned long)flags));
	check(last_write(WRITE_MIXER, &a, &b) && a == 128 && b == 64,
		"neutral release restages the mixer to the baseline legs",
		fmt("v1=%d v2=%d", a, b));

	status = run_op(SDK_OP_AUDIO_CONTROL_STATE_GET, &get, sizeof(get));
	check(status == SDK_STATUS_OK, "state get after release", NULL);
	check(w32(&result_buf[12]) == SDK_AUDIO_BALANCE_PACK(128, 64) &&
		w32(&result_buf[20]) == 0,
		"control state reports the baseline applied, unbounded",
		fmt("trim=0x%lx flags=0x%lx",
			(unsigned long)w32(&result_buf[12]),
			(unsigned long)w32(&result_buf[20])));

	/* Idempotent: the slot is already released, so a second neutral
	 * submit performs no mixer write and still reports baseline. */
	clear_writes();
	put32(tr.balance, SDK_AUDIO_BALANCE_NEUTRAL);
	check(run_op(SDK_OP_AUDIO_TRIM_SUBMIT, &tr, sizeof(tr)) ==
		SDK_STATUS_OK, "repeat neutral submit accepted", NULL);
	check(w32(&result_buf[0]) == SDK_AUDIO_BALANCE_PACK(128, 64),
		"repeat neutral still reports the baseline pair",
		fmt("applied=0x%lx", (unsigned long)w32(&result_buf[0])));
	check(write_count == 0,
		"repeat neutral release is write-free",
		fmt("writes=%d", write_count));
}

/* ---- meter read: framed snapshot through the dispatch ---- */

#define TEST_FRAMES 8

static int16_t out_period[TEST_FRAMES * 2];
static uint8_t cap_period[TEST_FRAMES * 2 * 2];

static void out_fill(int16_t left, int16_t right)
{
	int i;

	for (i = 0; i < TEST_FRAMES; i++) {
		out_period[i * 2] = left;
		out_period[i * 2 + 1] = right;
	}
}

static void cap_fill(int16_t left, int16_t right)
{
	int i;

	for (i = 0; i < TEST_FRAMES; i++) {
		cap_period[i * 4] = (uint8_t)(((uint16_t)left) >> 8);
		cap_period[i * 4 + 1] = (uint8_t)left;
		cap_period[i * 4 + 2] = (uint8_t)(((uint16_t)right) >> 8);
		cap_period[i * 4 + 3] = (uint8_t)right;
	}
}

static void test_meter_read(void)
{
	struct SDKAudioMeterReadPayload mr;
	uint16_t status;

	audio_scene_init();
	memset(&mr, 0, sizeof(mr));

	/* 0.5 / 0.375 FS -> 16.16 0x8000 / 0x6000 on the output. */
	out_fill(0x4000, 0x3000);
	audio_scene_meter_output_period(out_period, TEST_FRAMES);

	put32(mr.direction, SDK_AUDIO_METER_DIRECTION_OUTPUT);
	status = run_op(SDK_OP_AUDIO_METER_READ, &mr, sizeof(mr));
	check(status == SDK_STATUS_OK && result_len == sizeof(
			struct SDKAudioMeterResultPayload),
		"output meter read completes with result payload",
		fmt("status=%u len=%u", status, result_len));
	check(w32(&result_buf[0]) == SDK_AUDIO_METER_DIRECTION_OUTPUT,
		"meter read echoes direction", NULL);
	check(w32(&result_buf[8]) == 0 && w32(&result_buf[12]) == 1,
		"single-frame snapshot carries frame 0 of 1",
		fmt("frame=%lu count=%lu",
			(unsigned long)w32(&result_buf[8]),
			(unsigned long)w32(&result_buf[12])));
	check((w32(&result_buf[4]) & 1U) == 0,
		"snapshot generation is stable (non-tearing)",
		fmt("generation=%lu", (unsigned long)w32(&result_buf[4])));
	check(w32(&result_buf[40]) == 0x8000U &&
		w32(&result_buf[44]) == 0x6000U,
		"peaks reported in 16.16",
		fmt("ch1=0x%lx ch2=0x%lx",
			(unsigned long)w32(&result_buf[40]),
			(unsigned long)w32(&result_buf[44])));
	check(w32(&result_buf[20]) == SDK_AUDIO_METER_IDENTITY_UNKNOWN,
		"legacy output reports unknown identity", NULL);
	check(w32(&result_buf[24]) == 0,
		"no clips on the seeded period", NULL);

	/* Read-and-clear: the flags echo back and the next period opens
	 * a fresh hold window. */
	put32(mr.flags, SDK_AUDIO_METER_RESULT_HOLD_RESET);
	status = run_op(SDK_OP_AUDIO_METER_READ, &mr, sizeof(mr));
	check(status == SDK_STATUS_OK &&
		w32(&result_buf[16]) == SDK_AUDIO_METER_RESULT_HOLD_RESET,
		"hold-reset read reports the consumed window", NULL);
	out_fill(0x1000, 0x1000);
	audio_scene_meter_output_period(out_period, TEST_FRAMES);
	put32(mr.flags, 0);
	status = run_op(SDK_OP_AUDIO_METER_READ, &mr, sizeof(mr));
	check(status == SDK_STATUS_OK && w32(&result_buf[40]) == 0x2000U,
		"hold window restarted after consume",
		fmt("ch1=0x%lx", (unsigned long)w32(&result_buf[40])));

	/* Capture direction frames the same way. */
	cap_fill(0x2000, -0x2000);
	audio_scene_meter_capture_period(cap_period, TEST_FRAMES);
	put32(mr.direction, SDK_AUDIO_METER_DIRECTION_CAPTURE);
	status = run_op(SDK_OP_AUDIO_METER_READ, &mr, sizeof(mr));
	check(status == SDK_STATUS_OK &&
		w32(&result_buf[0]) == SDK_AUDIO_METER_DIRECTION_CAPTURE &&
		w32(&result_buf[40]) == 0x4000U &&
		w32(&result_buf[44]) == 0x4000U,
		"capture direction snapshot framed",
		fmt("dir=%lu ch1=0x%lx",
			(unsigned long)w32(&result_buf[0]),
			(unsigned long)w32(&result_buf[40])));
}

/* ---- scene save: validation synchronous, SD sequence QUEUED ---- */

static void test_scene_save(void)
{
	struct SDKAudioSceneWritePayload wr;
	struct SDKAudioSceneSavePayload save;
	struct SDKAudioControlStateGetPayload get;
	uint16_t status;

	audio_scene_init();
	mock_fs_reset();
	memset(&wr, 0, sizeof(wr));
	memset(&save, 0, sizeof(save));
	memset(&get, 0, sizeof(get));

	/* Store an over-boundary scene in an inactive slot (staging +
	 * commit never touches the DSP for an inactive scene). */
	put32(wr.scene, 1);
	put32(wr.param, SDK_AUDIO_SCENE_PARAM_PREFACTOR);
	put32(wr.value, 100);
	put32(wr.flags, SDK_AUDIO_SCENE_WRITE_FLAG_COMMIT);
	status = run_op(SDK_OP_AUDIO_SCENE_WRITE, &wr, sizeof(wr));
	check(status == SDK_STATUS_OK, "stage over-boundary scene", NULL);
	check(audio_scene_get(1) != NULL &&
		audio_scene_get(1)->prefactor == 100,
		"over-boundary scene stored in inactive slot", NULL);

	/* Validation stays synchronous: the rejection answers the save
	 * call itself, before any SD traffic. */
	put32(save.scene, 1);
	status = run_op(SDK_OP_AUDIO_SCENE_SAVE, &save, sizeof(save));
	check(status == SDK_STATUS_OK && result_len == sizeof(
			struct SDKAudioSceneSaveResultPayload),
		"scene save completes with result payload",
		fmt("status=%u len=%u", status, result_len));
	check(w32(&result_buf[0]) == SDK_AUDIO_SCENE_SAVE_REJECTED &&
		w32(&result_buf[4]) == 1,
		"save rejects a scene above the enforced boundary",
		fmt("status=%lu scene=%lu",
			(unsigned long)w32(&result_buf[0]),
			(unsigned long)w32(&result_buf[4])));

	/* The writer persists every slot, so the over-boundary scene in
	 * slot 1 rejects a save of slot 0 too (cross-slot rule). */
	put32(save.scene, 0);
	status = run_op(SDK_OP_AUDIO_SCENE_SAVE, &save, sizeof(save));
	check(status == SDK_STATUS_OK &&
		w32(&result_buf[0]) == SDK_AUDIO_SCENE_SAVE_REJECTED &&
		w32(&result_buf[4]) == 0,
		"save rejects any slot while one scene is over-boundary",
		fmt("status=%lu scene=%lu",
			(unsigned long)w32(&result_buf[0]),
			(unsigned long)w32(&result_buf[4])));

	/* Back within bounds everywhere. Start a real active-scene DSP
	 * commit first: SCENE_SAVE must queue behind it without draining
	 * any I2C work inside the mailbox request. */
	audio_scene_init();
	clear_writes();
	memset(&wr, 0, sizeof(wr));
	put32(wr.scene, 0);
	put32(wr.param, SDK_AUDIO_SCENE_PARAM_VOLUME);
	put32(wr.value, 70);
	put32(wr.flags, SDK_AUDIO_SCENE_WRITE_FLAG_COMMIT);
	check(run_op(SDK_OP_AUDIO_SCENE_WRITE, &wr, sizeof(wr)) ==
		SDK_STATUS_OK, "active edit starts before save", NULL);
	status = run_op(SDK_OP_AUDIO_SCENE_SAVE, &save, sizeof(save));
	check(write_count == 0,
		"save dispatch does not synchronously drain the DSP commit",
		fmt("writes=%d", write_count));
	check(status == SDK_STATUS_OK &&
		w32(&result_buf[0]) == SDK_AUDIO_SCENE_SAVE_QUEUED &&
		w32(&result_buf[4]) == 0,
		"save starts the machine and replies QUEUED",
		fmt("status=%lu scene=%lu",
			(unsigned long)w32(&result_buf[0]),
			(unsigned long)w32(&result_buf[4])));

	/* A second save while the machine runs is refused BUSY. */
	status = run_op(SDK_OP_AUDIO_SCENE_SAVE, &save, sizeof(save));
	check(status == SDK_STATUS_OK &&
		w32(&result_buf[0]) == SDK_AUDIO_SCENE_SAVE_BUSY,
		"second save while running replies BUSY",
		fmt("status=%lu", (unsigned long)w32(&result_buf[0])));

	/* The control-state report carries the running save status at
	 * its append-only offset (44) while the machine is mid-flight. */
	status = run_op(SDK_OP_AUDIO_CONTROL_STATE_GET, &get, sizeof(get));
	check(status == SDK_STATUS_OK &&
		w32(&result_buf[44]) == SDK_AUDIO_SCENE_SAVE_QUEUED,
		"control state reports the save in progress",
		fmt("save_status=%lu", (unsigned long)w32(&result_buf[44])));

	/* Driving the poll to idle (the main service loop) completes the
	 * save; the settled status and the persisted file prove it. */
	pump_scene();
	status = run_op(SDK_OP_AUDIO_CONTROL_STATE_GET, &get, sizeof(get));
	check(status == SDK_STATUS_OK &&
		w32(&result_buf[44]) == SDK_AUDIO_SCENE_SAVE_OK,
		"control state reports the settled save",
		fmt("save_status=%lu", (unsigned long)w32(&result_buf[44])));
	check(mock_fs_file("0:/ZZ9000.CFG") != NULL &&
			strstr(mock_fs_file("0:/ZZ9000.CFG"),
				"audio_scene0_out = 6470\n") != NULL,
		"save snapshots the scene only after its DSP commit settles",
		NULL);
	check(mock_fs_file("0:/ZZCFG.TMP") == NULL,
		"settled save leaves no temp", NULL);

	/* An injected mid-write failure settles IO_ERROR through the same
	 * report, with the original intact and the temp left behind. */
	mock_fs_reset();
	mock_fs_set_file("0:/ZZ9000.CFG", "int2 = on\n");
	mock_fail_write(1);
	status = run_op(SDK_OP_AUDIO_SCENE_SAVE, &save, sizeof(save));
	check(status == SDK_STATUS_OK &&
		w32(&result_buf[0]) == SDK_AUDIO_SCENE_SAVE_QUEUED,
		"failing save still starts and replies QUEUED",
		fmt("status=%lu", (unsigned long)w32(&result_buf[0])));
	pump_scene();
	status = run_op(SDK_OP_AUDIO_CONTROL_STATE_GET, &get, sizeof(get));
	check(status == SDK_STATUS_OK &&
		w32(&result_buf[44]) == SDK_AUDIO_SCENE_SAVE_IO_ERROR,
		"control state reports the IO failure",
		fmt("save_status=%lu", (unsigned long)w32(&result_buf[44])));
	check(mock_fs_file("0:/ZZ9000.CFG") != NULL &&
		strcmp(mock_fs_file("0:/ZZ9000.CFG"), "int2 = on\n") == 0,
		"original intact after mid-write failure", NULL);
	check(mock_fs_file("0:/ZZCFG.TMP") != NULL,
		"partial temp left for the reset hook", NULL);
	mock_fail_write(0);
}


static void test_baseline_write_persists_through_queued_save(void)
{
	struct SDKAudioSceneWritePayload wr;
	struct SDKAudioSceneSavePayload save;
	struct SDKAudioControlStateGetPayload get;
	uint16_t status;

	audio_scene_init();
	mock_fs_reset();
	memset(&wr, 0, sizeof(wr));
	memset(&save, 0, sizeof(save));
	memset(&get, 0, sizeof(get));

	put32(wr.scene, 0);
	put32(wr.param, SDK_AUDIO_SCENE_PARAM_BASELINE);
	put32(wr.value, SDK_AUDIO_BALANCE_PACK(128, 63));
	put32(wr.flags, SDK_AUDIO_SCENE_WRITE_FLAG_COMMIT);
	check(run_op(SDK_OP_AUDIO_SCENE_WRITE, &wr, sizeof(wr)) ==
		SDK_STATUS_OK, "UI baseline write accepted", NULL);
	check(run_op(SDK_OP_AUDIO_CONTROL_STATE_GET, &get, sizeof(get)) ==
			SDK_STATUS_OK &&
			w32(&result_buf[8]) == SDK_AUDIO_BALANCE_PACK(128, 63),
		"control state owns the edited baseline before Save", NULL);

	status = run_op(SDK_OP_AUDIO_SCENE_SAVE, &save, sizeof(save));
	check(status == SDK_STATUS_OK &&
			w32(&result_buf[0]) == SDK_AUDIO_SCENE_SAVE_QUEUED,
		"immediate Save queues behind the baseline commit", NULL);
	pump_scene();
	check(audio_scene_save_status() == SDK_AUDIO_SCENE_SAVE_OK,
		"baseline Save settles OK", NULL);
	check(mock_fs_file("0:/ZZ9000.CFG") != NULL &&
			strstr(mock_fs_file("0:/ZZ9000.CFG"),
				"audio_baseline = 32831\n") != NULL,
		"queued Save persists the edited baseline", NULL);
	check(run_op(SDK_OP_AUDIO_CONTROL_STATE_GET, &get, sizeof(get)) ==
			SDK_STATUS_OK &&
			w32(&result_buf[8]) == SDK_AUDIO_BALANCE_PACK(128, 63),
		"control state retains the baseline after Save", NULL);
}

static void test_scene_rename_persists_through_queued_save(void)
{
	static const uint32_t chunks[8] = {
		0x4265, 0x6e63, 0x6800, 0, 0, 0, 0, 0 /* "Bench" */
	};
	struct SDKAudioSceneWritePayload wr;
	struct SDKAudioSceneSavePayload save;
	uint16_t status;
	int i;

	audio_scene_init();
	mock_fs_reset();
	memset(&wr, 0, sizeof(wr));
	memset(&save, 0, sizeof(save));
	put32(wr.scene, 0);
	put32(wr.param, SDK_AUDIO_SCENE_PARAM_NAME);

	/* ZZTop's retry-safe rename shape: terminator guard, then all
	 * eight chunks with COMMIT on the final call only. */
	check(run_op(SDK_OP_AUDIO_SCENE_WRITE, &wr, sizeof(wr)) ==
		SDK_STATUS_OK, "rename guard accepted", NULL);
	for (i = 0; i < 8; i++) {
		put32(wr.value, chunks[i]);
		put32(wr.flags, (i == 7) ?
			SDK_AUDIO_SCENE_WRITE_FLAG_COMMIT : 0);
		check(run_op(SDK_OP_AUDIO_SCENE_WRITE, &wr, sizeof(wr)) ==
			SDK_STATUS_OK, "rename chunk accepted", NULL);
	}
	check(audio_scene_get(0) != NULL &&
			strcmp(audio_scene_get(0)->name, "Bench") == 0,
		"firmware scene table owns the renamed label", NULL);

	status = run_op(SDK_OP_AUDIO_SCENE_SAVE, &save, sizeof(save));
	check(status == SDK_STATUS_OK &&
			w32(&result_buf[0]) == SDK_AUDIO_SCENE_SAVE_QUEUED,
		"immediate Save queues behind the name commit", NULL);
	pump_scene();
	check(audio_scene_save_status() == SDK_AUDIO_SCENE_SAVE_OK,
		"rename Save settles OK", NULL);
	check(mock_fs_file("0:/ZZ9000.CFG") != NULL &&
			strstr(mock_fs_file("0:/ZZ9000.CFG"),
				"audio_scene0_nm1 = 16997\n") != NULL &&
			strstr(mock_fs_file("0:/ZZ9000.CFG"),
				"audio_scene0_nm2 = 28259\n") != NULL &&
			strstr(mock_fs_file("0:/ZZ9000.CFG"),
				"audio_scene0_nm3 = 26624\n") != NULL,
		"queued Save persists the renamed label", NULL);
}

/* ---- staged baseline commit rides the same commit path ---- */

static void test_baseline_write_path(void)
{
	struct SDKAudioSceneWritePayload wr;
	struct SDKAudioControlStateGetPayload get;
	int a = 0, b = 0;
	int ok;

	audio_scene_init();
	clear_writes();
	memset(&wr, 0, sizeof(wr));
	memset(&get, 0, sizeof(get));

	put32(wr.scene, 5); /* ignored for BASELINE */
	put32(wr.param, SDK_AUDIO_SCENE_PARAM_BASELINE);
	put32(wr.value, SDK_AUDIO_BALANCE_PACK(150, 40));
	put32(wr.flags, SDK_AUDIO_SCENE_WRITE_FLAG_COMMIT);
	check(run_op(SDK_OP_AUDIO_SCENE_WRITE, &wr, sizeof(wr)) ==
		SDK_STATUS_OK, "baseline stage+commit", NULL);
	check(audio_scene_baseline_paula() == 150 &&
		audio_scene_baseline_ax() == 40,
		"baseline stored", NULL);
	pump_scene();
	/* The staged baseline is a live edit: the differential commit
	 * moves only the mixer legs (the scene's parameters and its
	 * resolved output volume are unchanged). */
	check(write_count == 6,
		"baseline diff: 2 legs x 3 safeload substeps",
		fmt("writes=%d", write_count));
	ok = last_write(WRITE_MIXER_A, &a, &b) && a == 40;
	check(ok, "baseline diff lands the AX mixer leg last",
		fmt("leg=%d", a));
	check(audio_scene_gain_reduction_events() == 0,
		"within-boundary baseline emits no event", NULL);

	check(run_op(SDK_OP_AUDIO_CONTROL_STATE_GET, &get, sizeof(get)) ==
		SDK_STATUS_OK, "state get after baseline", NULL);
	check(w32(&result_buf[8]) == SDK_AUDIO_BALANCE_PACK(150, 40),
		"state get reports the committed baseline",
		fmt("baseline=0x%lx",
			(unsigned long)w32(&result_buf[8])));

	/* BASELINE ignores the scene index at stage time; the COMMIT
	 * still needs a valid scene slot. */
	audio_scene_init();
	memset(&wr, 0, sizeof(wr));
	put32(wr.scene, 8);
	put32(wr.param, SDK_AUDIO_SCENE_PARAM_BASELINE);
	put32(wr.value, SDK_AUDIO_BALANCE_PACK(150, 40));
	check(run_op(SDK_OP_AUDIO_SCENE_WRITE, &wr, sizeof(wr)) ==
		SDK_STATUS_OK,
		"baseline stage ignores the scene index", NULL);
	put32(wr.flags, SDK_AUDIO_SCENE_WRITE_FLAG_COMMIT);
	check(run_op(SDK_OP_AUDIO_SCENE_WRITE, &wr, sizeof(wr)) ==
		SDK_STATUS_BAD_REQUEST,
		"commit requires a valid scene slot", NULL);
}

static void test_calibration_write_path(void)
{
	struct SDKAudioSceneWritePayload wr;
	struct SDKAudioControlStateGetPayload get;

	audio_scene_init();
	memset(&wr, 0, sizeof(wr));
	memset(&get, 0, sizeof(get));
	put32(wr.scene, 6); /* ignored for CALIBRATION */
	put32(wr.param, SDK_AUDIO_SCENE_PARAM_CALIBRATION);
	put32(wr.value, SDK_AUDIO_CALIBRATION_PACK(48, 80));
	put32(wr.flags, SDK_AUDIO_SCENE_WRITE_FLAG_COMMIT);
	check(run_op(SDK_OP_AUDIO_SCENE_WRITE, &wr, sizeof(wr)) ==
		SDK_STATUS_OK, "calibration stage+commit", NULL);
	pump_scene();
	check(audio_scene_ceiling_paula() == 48 &&
		audio_scene_ceiling_ax() == 80,
		"calibration stored", NULL);
	check(run_op(SDK_OP_AUDIO_CONTROL_STATE_GET, &get, sizeof(get)) ==
		SDK_STATUS_OK, "state get after calibration", NULL);
	check(w32(&result_buf[16]) == 60 &&
		w32(&result_buf[24]) == 48 &&
		w32(&result_buf[28]) == 80,
		"state reports measured ceilings and derived boundary",
		fmt("boundary=%lu p=%lu ax=%lu",
			(unsigned long)w32(&result_buf[16]),
			(unsigned long)w32(&result_buf[24]),
			(unsigned long)w32(&result_buf[28])));

	memset(&wr, 0, sizeof(wr));
	put32(wr.param, SDK_AUDIO_SCENE_PARAM_CALIBRATION);
	put32(wr.value, SDK_AUDIO_CALIBRATION_PACK(0, 80));
	check(run_op(SDK_OP_AUDIO_SCENE_WRITE, &wr, sizeof(wr)) ==
		SDK_STATUS_BAD_REQUEST,
		"zero calibration ceiling rejected", NULL);
}

/* ---- unknown opcodes and request validation ---- */

static void test_unsupported_and_validation(void)
{
	struct SDKAudioSceneSelectPayload sel;
	struct SDKAudioSceneWritePayload wr;
	struct SDKAudioMeterReadPayload mr;
	uint8_t short_buf[48];
	int a = -1, b = -1;

	audio_scene_init();
	memset(&sel, 0, sizeof(sel));
	memset(&wr, 0, sizeof(wr));
	memset(&mr, 0, sizeof(mr));
	memset(short_buf, 0, sizeof(short_buf));

	/* Unassigned audio-service opcodes still complete UNSUPPORTED,
	 * exactly as the mailbox default does for them today. */
	check(run_op(0x050f, short_buf, 48) == SDK_STATUS_UNSUPPORTED,
		"unassigned audio opcode 0x050f unsupported", NULL);
	check(run_op(0x05ff, short_buf, 48) == SDK_STATUS_UNSUPPORTED,
		"unassigned audio opcode 0x05ff unsupported", NULL);

	/* Short payloads are BAD_REQUEST for every opcode. */
	check(run_op(SDK_OP_AUDIO_SCENE_SELECT, short_buf, 3) ==
		SDK_STATUS_BAD_REQUEST,
		"short scene select payload rejected", NULL);
	check(run_op(SDK_OP_AUDIO_SCENE_WRITE, short_buf, 47) ==
		SDK_STATUS_BAD_REQUEST,
		"short scene write payload rejected", NULL);
	check(run_op(SDK_OP_AUDIO_TRIM_SUBMIT, short_buf, 16) ==
		SDK_STATUS_BAD_REQUEST,
		"short trim submit payload rejected", NULL);
	check(run_op(SDK_OP_AUDIO_METER_READ, short_buf, 0) ==
		SDK_STATUS_BAD_REQUEST,
		"short meter read payload rejected", NULL);
	check(run_op(SDK_OP_AUDIO_SCENE_SAVE, short_buf, 7) ==
		SDK_STATUS_BAD_REQUEST,
		"short scene save payload rejected", NULL);
	check(run_op(SDK_OP_AUDIO_CONTROL_STATE_GET, short_buf, 4) ==
		SDK_STATUS_BAD_REQUEST,
		"short control state payload rejected", NULL);

	/* Range and flag validation. */
	put32(sel.scene, 8);
	check(run_op(SDK_OP_AUDIO_SCENE_SELECT, &sel, sizeof(sel)) ==
		SDK_STATUS_BAD_REQUEST,
		"select rejects out-of-range scene", NULL);
	put32(sel.scene, 0);
	put32(sel.flags, 1);
	check(run_op(SDK_OP_AUDIO_SCENE_SELECT, &sel, sizeof(sel)) ==
		SDK_STATUS_BAD_REQUEST,
		"select rejects unknown flags", NULL);

	put32(wr.scene, 8);
	put32(wr.param, SDK_AUDIO_SCENE_PARAM_VOLUME);
	put32(wr.value, 50);
	check(run_op(SDK_OP_AUDIO_SCENE_WRITE, &wr, sizeof(wr)) ==
		SDK_STATUS_BAD_REQUEST,
		"scene write rejects out-of-range scene slot", NULL);
	/* 256 truncates to slot 0 as a uint8_t index: it must be
	 * rejected on the full 32-bit value and stage nothing -- a
	 * later pan-only edit must commit with the untouched default
	 * volume, not a smuggled volume 50. */
	clear_writes();
	put32(wr.scene, 256);
	check(run_op(SDK_OP_AUDIO_SCENE_WRITE, &wr, sizeof(wr)) ==
		SDK_STATUS_BAD_REQUEST,
		"stage rejects a truncating 32-bit scene index", NULL);
	put32(wr.scene, 0);
	put32(wr.param, SDK_AUDIO_SCENE_PARAM_PAN);
	put32(wr.value, 60);
	put32(wr.flags, SDK_AUDIO_SCENE_WRITE_FLAG_COMMIT);
	check(run_op(SDK_OP_AUDIO_SCENE_WRITE, &wr, sizeof(wr)) ==
		SDK_STATUS_OK, "pan-only edit commits", NULL);
	pump_scene();
	check(last_write(WRITE_VOLPAN_SIDE1, &a, &b) && a == 100 && b == 60,
		"rejected stage left no draft behind",
		fmt("vol=%d pan=%d", a, b));
	put32(wr.flags, 1U << 1);
	check(run_op(SDK_OP_AUDIO_SCENE_WRITE, &wr, sizeof(wr)) ==
		SDK_STATUS_BAD_REQUEST,
		"scene write rejects unknown flags", NULL);
	put32(wr.param, SDK_AUDIO_SCENE_PARAM_LPF);
	put32(wr.value, 0);
	check(run_op(SDK_OP_AUDIO_SCENE_WRITE, &wr, sizeof(wr)) ==
		SDK_STATUS_BAD_REQUEST,
		"scene write rejects LPF below the DSP range", NULL);
	put32(wr.param, SDK_AUDIO_SCENE_PARAM_PAN);
	put32(wr.value, 101);
	check(run_op(SDK_OP_AUDIO_SCENE_WRITE, &wr, sizeof(wr)) ==
		SDK_STATUS_BAD_REQUEST,
		"scene write rejects pan above 100", NULL);

	put32(mr.direction, 3);
	check(run_op(SDK_OP_AUDIO_METER_READ, &mr, sizeof(mr)) ==
		SDK_STATUS_BAD_REQUEST,
		"meter read rejects unknown direction", NULL);
	put32(mr.direction, 0);
	check(run_op(SDK_OP_AUDIO_METER_READ, &mr, sizeof(mr)) ==
		SDK_STATUS_BAD_REQUEST,
		"meter read rejects direction 0", NULL);
	put32(mr.direction, SDK_AUDIO_METER_DIRECTION_OUTPUT);
	put32(mr.flags, 1U << 1);
	check(run_op(SDK_OP_AUDIO_METER_READ, &mr, sizeof(mr)) ==
		SDK_STATUS_BAD_REQUEST,
		"meter read rejects unknown flags", NULL);
}

int main(void)
{
	test_select_state_roundtrip();
	test_staged_write_accumulates();
	test_commit_ordering();
	test_rapid_switches_serialize();
	test_nested_commit_coalesces();
	test_trim_submit_result();
	test_trim_neutral_keep_baseline();
	test_trim_neutral_releases_held_trim();
	test_meter_read();
	test_scene_save();
	test_baseline_write_persists_through_queued_save();
	test_scene_rename_persists_through_queued_save();
	test_baseline_write_path();
	test_calibration_write_path();
	test_unsupported_and_validation();

	if (failures == 0) {
		printf("audio_control_test: all tests passed\n");
		return 0;
	}
	printf("audio_control_test: %d failure(s)\n", failures);
	return 1;
}
