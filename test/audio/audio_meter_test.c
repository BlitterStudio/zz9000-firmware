/*
 * Host tests for the audio control-plane metering accumulators and
 * coherent snapshots (plan U3, KTD3): peak-hold-since-last-read
 * consume semantics with the HOLD_RESET opt-out, at-rail clip regions,
 * saturating counters, producer-stagnation underruns for register-fed
 * playback, per-direction identity reporting, non-tearing snapshots
 * under a concurrent single writer, and the framed
 * SDKAudioMeterResultPayload packing.
 *
 * This harness includes audio_scene.c directly (single translation
 * unit) so the saturating-counter guards can be seeded near their
 * boundary; the ax.h DSP setters the arbiter calls are provided as
 * no-op stubs, matching the audio_scene_test link-time seam.
 */

#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include "audio_scene.h"
#include "memorymap.h"
#include "sdk_mailbox.h"

#include "audio_scene.c"

/* ---- no-op stubs for the ax.h DSP setters ---- */

int audio_adau_set_lpf_params(int f0) { (void)f0; return 0; }
int audio_adau_set_mixer_vol(int vol1, int vol2)
{
	(void)vol1; (void)vol2; return 0;
}
int audio_adau_set_prefactor(int pre) { (void)pre; return 0; }
int audio_adau_set_eq_gain(int band, int gain)
{
	(void)band; (void)gain; return 0;
}
int audio_adau_set_vol_pan(int vol, int pan)
{
	(void)vol; (void)pan; return 0;
}
int audio_adau_set_vol_pan_side(int side, int vol, int pan)
{
	(void)side; (void)vol; (void)pan; return 0;
}

int audio_adau_eq_substep(int band, int gain, int substep)
{
	(void)band; (void)gain;
	if (substep < 10) return 0;
	return 1;
}
__attribute__((unused)) int audio_adau_safe_mixer_leg(int leg, int value, int substep)
{
	(void)leg; (void)value;
	return (substep < 2) ? 0 : 1;
}

__attribute__((unused)) int audio_adau_safe_vol_pan_side(int side, int vol, int pan, int substep)
{
	(void)side; (void)vol; (void)pan;
	return (substep < 2) ? 0 : 1;
}

__attribute__((unused)) int audio_adau_safe_prefactor(int pre, int substep)
{
	(void)pre;
	return (substep < 4) ? 0 : 1;
}

int audio_adau_set_mixer_leg(int leg, int value)
{
	(void)leg; (void)value; return 0;
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

/* ---- fixtures ---- */

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

static void be_put(uint8_t *p, int16_t v)
{
	p[0] = (uint8_t)(((uint16_t)v) >> 8);
	p[1] = (uint8_t)v;
}

static void cap_fill(int16_t left, int16_t right)
{
	int i;
	for (i = 0; i < TEST_FRAMES; i++) {
		be_put(&cap_period[i * 4], left);
		be_put(&cap_period[i * 4 + 2], right);
	}
}

/* Big-endian word getter: reads the payload exactly like an SDK
 * client (abi.h accessor) would, proving the firmware packing order. */
static uint32_t w32(const uint8_t p[4])
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static struct audio_meter_snapshot snap;

static uint32_t read_output(uint32_t flags)
{
	if (audio_scene_meter_read(SDK_AUDIO_METER_DIRECTION_OUTPUT,
	                           flags, &snap) != 0)
		return 0xdeadbeefU;
	return snap.peak_hold_ch1;
}

/* ---- peak-hold-since-last-read (R8) ---- */

static void test_peak_hold_consume_reset(void)
{
	audio_scene_init();

	/* 0.5 FS on ch1, 0.375 FS on ch2 -> 16.16 0x8000 / 0x6000. */
	out_fill(0x4000, 0x3000);
	audio_scene_meter_output_period(out_period, TEST_FRAMES);
	check(read_output(SDK_AUDIO_METER_RESULT_HOLD_RESET) == 0x8000U &&
	      snap.peak_hold_ch2 == 0x6000U,
		"hold reports the period peak in 16.16",
		fmt("ch1=%08lx ch2=%08lx",
		    (unsigned long)snap.peak_hold_ch1,
		    (unsigned long)snap.peak_hold_ch2));

	/* Consume-on-read: the next window starts fresh, so a smaller
	 * peak is not max-merged with the consumed one. */
	out_fill(0x1000, 0x0800);
	audio_scene_meter_output_period(out_period, TEST_FRAMES);
	check(read_output(SDK_AUDIO_METER_RESULT_HOLD_RESET) == 0x2000U &&
	      snap.peak_hold_ch2 == 0x1000U,
		"consumed hold does not leak into the next window",
		fmt("ch1=%08lx", (unsigned long)snap.peak_hold_ch1));

	/* Opt-out: a read without HOLD_RESET peeks without consuming.
	 * The previous consuming read already marked the boundary, so
	 * this feed opened a fresh window at 0x80. */
	out_fill(0x0040, 0x0040);
	audio_scene_meter_output_period(out_period, TEST_FRAMES);
	check(read_output(0) == 0x80U,
		"read without HOLD_RESET peeks the current window",
		fmt("ch1=%08lx", (unsigned long)snap.peak_hold_ch1));

	/* The peek consumed nothing: a smaller peak still max-merges
	 * into the same window. */
	out_fill(0x0020, 0x0020);
	audio_scene_meter_output_period(out_period, TEST_FRAMES);
	check(read_output(SDK_AUDIO_METER_RESULT_HOLD_RESET) == 0x80U,
		"held peak survives a peeking read",
		fmt("ch1=%08lx", (unsigned long)snap.peak_hold_ch1));

	/* Only after the consuming read does the window restart. */
	out_fill(0x0010, 0x0010);
	audio_scene_meter_output_period(out_period, TEST_FRAMES);
	check(read_output(0) == 0x20U,
		"consuming read restarts the hold window",
		fmt("ch1=%08lx", (unsigned long)snap.peak_hold_ch1));

	/* Reads are pure observers of the counters: nothing but the
	 * hold window is disturbed. */
	{
		struct audio_meter_snapshot before;

		audio_scene_meter_output_underrun();
		read_output(0);
		before = snap;
		read_output(0);
		read_output(SDK_AUDIO_METER_RESULT_HOLD_RESET);
		check(snap.underrun_count == before.underrun_count &&
		      snap.clip_count == before.clip_count,
			"meter reads disturb no counter", NULL);
	}

	/* Direction validation. */
	check(audio_scene_meter_read(0, 0, &snap) == -1 &&
	      audio_scene_meter_read(3, 0, &snap) == -1 &&
	      audio_scene_meter_read(SDK_AUDIO_METER_DIRECTION_OUTPUT,
	                             0, NULL) == -1,
		"meter read rejects bad direction and NULL snapshot", NULL);
}

/* ---- clip counting: once per saturating region (R8) ---- */

static void test_clip_regions(void)
{
	audio_scene_init();

	/* Two separated at-rail runs (5 rail samples total) and a rail
	 * on the opposite channel: exactly two regions. */
	memset(out_period, 0, sizeof(out_period));
	out_period[0] = INT16_MAX;
	out_period[2] = INT16_MAX;
	out_period[4] = INT16_MAX;
	out_period[8] = INT16_MIN;  /* frame 4, left */
	out_period[11] = INT16_MIN; /* frame 5, right */
	audio_scene_meter_output_period(out_period, TEST_FRAMES);
	check(read_output(0) == 0x10000U,
		"full-scale rail peak reads as 0x10000",
		fmt("ch1=%08lx", (unsigned long)snap.peak_hold_ch1));
	check(snap.clip_count == 2,
		"clip count increments once per region, not per sample",
		fmt("clips=%08lx", (unsigned long)snap.clip_count));

	/* A region spanning a period boundary is still one region. */
	memset(out_period, 0, sizeof(out_period));
	out_period[(TEST_FRAMES - 1) * 2] = INT16_MAX;
	audio_scene_meter_output_period(out_period, TEST_FRAMES);
	memset(out_period, 0, sizeof(out_period));
	out_period[1] = INT16_MAX;
	out_period[4] = INT16_MAX;
	audio_scene_meter_output_period(out_period, TEST_FRAMES);
	read_output(0);
	check(snap.clip_count == 4,
		"rail run across a period boundary counts once",
		fmt("clips=%08lx", (unsigned long)snap.clip_count));

	/* Full-period silence closes the region; a later rail opens a
	 * new one. */
	out_fill(0, 0);
	audio_scene_meter_output_period(out_period, TEST_FRAMES);
	out_fill(1, 1);
	audio_scene_meter_output_period(out_period, TEST_FRAMES);
	out_fill(0, -1);
	audio_scene_meter_output_period(out_period, TEST_FRAMES);
	out_fill(0, INT16_MIN);
	audio_scene_meter_output_period(out_period, TEST_FRAMES);
	read_output(0);
	check(snap.clip_count == 5,
		"separated rails count separately",
		fmt("clips=%08lx", (unsigned long)snap.clip_count));
}

/* ---- capture direction: big-endian published periods ---- */

static void test_capture_meter(void)
{
	audio_scene_init();

	cap_fill(0x2000, -0x2000);
	audio_scene_meter_capture_period(cap_period, TEST_FRAMES);
	check(audio_scene_meter_read(SDK_AUDIO_METER_DIRECTION_CAPTURE,
	                             0, &snap) == 0 &&
	      snap.peak_hold_ch1 == 0x4000U &&
	      snap.peak_hold_ch2 == 0x4000U,
		"capture peak decodes published S16BE periods",
		fmt("ch1=%08lx ch2=%08lx",
		    (unsigned long)snap.peak_hold_ch1,
		    (unsigned long)snap.peak_hold_ch2));

	be_put(&cap_period[0], INT16_MAX);
	be_put(&cap_period[5], INT16_MAX);
	audio_scene_meter_capture_period(cap_period, TEST_FRAMES);
	audio_scene_meter_capture_overrun();
	check(audio_scene_meter_read(SDK_AUDIO_METER_DIRECTION_CAPTURE,
	                             0, &snap) == 0 &&
	      snap.clip_count == 1 && snap.overrun_count == 1,
		"capture clip regions and overrun accumulate",
		fmt("clips=%08lx overruns=%08lx",
		    (unsigned long)snap.clip_count,
		    (unsigned long)snap.overrun_count));

	/* Direction independence: capture events never touch output. */
	check(audio_scene_meter_read(SDK_AUDIO_METER_DIRECTION_OUTPUT,
	                             0, &snap) == 0 &&
	      snap.clip_count == 0 && snap.overrun_count == 0 &&
	      snap.peak_hold_ch1 == 0 && snap.underrun_count == 0,
		"directions accumulate independently", NULL);

	/* Gain-reduction events belong to the output path only. */
	check(snap.gain_reduction_events == 0,
		"capture snapshot reports no gain events", NULL);
}

/* ---- counters saturate, never wrap ---- */

static void test_counter_saturation(void)
{
	audio_scene_init();

	meters[METER_OUTPUT].underrun_count = UINT32_MAX - 1U;
	audio_scene_meter_output_underrun();
	audio_scene_meter_output_underrun();
	audio_scene_meter_output_underrun();
	read_output(0);
	check(snap.underrun_count == UINT32_MAX,
		"underrun counter saturates without wrapping",
		fmt("underruns=%08lx", (unsigned long)snap.underrun_count));

	meters[METER_CAPTURE].overrun_count = UINT32_MAX - 1U;
	audio_scene_meter_capture_overrun();
	audio_scene_meter_capture_overrun();
	audio_scene_meter_read(SDK_AUDIO_METER_DIRECTION_CAPTURE, 0, &snap);
	check(snap.overrun_count == UINT32_MAX,
		"overrun counter saturates without wrapping", NULL);

	/* Two clip regions in one period against a near-max counter:
	 * saturating add, not modulo add. */
	meters[METER_OUTPUT].clip_count = UINT32_MAX - 1U;
	meters[METER_OUTPUT].clip_open = 0;
	memset(out_period, 0, sizeof(out_period));
	out_period[0] = INT16_MAX;
	out_period[4] = INT16_MAX;
	audio_scene_meter_output_period(out_period, TEST_FRAMES);
	read_output(0);
	check(snap.clip_count == UINT32_MAX,
		"clip counter saturates mid-period",
		fmt("clips=%08lx", (unsigned long)snap.clip_count));
}

/* ---- producer-stagnation underruns (register-fed playback) ---- */

static void test_producer_stagnation(void)
{
	audio_scene_init();

	/* Playback enabled at producer sequence 100: the pre-filled
	 * ring must not count as underruns before the producer's first
	 * refill completes a period. */
	audio_scene_meter_output_producer_arm(100U);
	audio_scene_meter_output_producer_tick(100U);
	audio_scene_meter_output_producer_tick(100U);
	read_output(0);
	check(snap.underrun_count == 0,
		"startup fills never count as underruns", NULL);

	/* First producer refill arms stagnation detection. */
	audio_scene_meter_output_producer_tick(101U);
	check(snap.underrun_count == 0,
		"producer advance is not an underrun", NULL);

	/* Stagnation: each completed period with an unchanged producer
	 * sequence played unfilled audio. */
	audio_scene_meter_output_producer_tick(101U);
	audio_scene_meter_output_producer_tick(101U);
	read_output(0);
	check(snap.underrun_count == 2,
		"one underrun per stagnant completed period",
		fmt("underruns=%08lx", (unsigned long)snap.underrun_count));

	/* Recovery, then a fresh single miss. */
	audio_scene_meter_output_producer_tick(102U);
	audio_scene_meter_output_producer_tick(103U);
	read_output(0);
	check(snap.underrun_count == 2,
		"advancing producer stops the counting", NULL);
	audio_scene_meter_output_producer_tick(103U);
	read_output(0);
	check(snap.underrun_count == 3,
		"stagnation re-arms after recovery", NULL);

	/* Re-arming for a new playback resets the detector. */
	audio_scene_meter_output_producer_arm(200U);
	audio_scene_meter_output_producer_tick(200U);
	read_output(0);
	check(snap.underrun_count == 3,
		"re-arm after playback restart waits for a refill", NULL);
}

/* ---- identity (R8): named participants, legacy unknown ---- */

static void test_identity(void)
{
	audio_scene_init();

	read_output(0);
	check(snap.identity == SDK_AUDIO_METER_IDENTITY_UNKNOWN,
		"idle output reports unknown identity", NULL);
	audio_scene_meter_read(SDK_AUDIO_METER_DIRECTION_CAPTURE, 0, &snap);
	check(snap.identity == SDK_AUDIO_METER_IDENTITY_UNKNOWN,
		"non-participating capture reports unknown identity", NULL);

	/* Control-plane session bindings name the output source. */
	audio_scene_meter_output_identity(SDK_AUDIO_METER_IDENTITY_MEDIA);
	read_output(0);
	check(snap.identity == SDK_AUDIO_METER_IDENTITY_MEDIA,
		"media pump binding names the output", NULL);
	audio_scene_meter_output_identity(
		SDK_AUDIO_METER_IDENTITY_SDK_STREAM);
	read_output(0);
	check(snap.identity == SDK_AUDIO_METER_IDENTITY_SDK_STREAM,
		"sdk stream pump binding names the output", NULL);

	/* Register-fed playback is the legacy/unknown default; meters
	 * still accumulate underneath it. */
	audio_scene_meter_output_identity(
		SDK_AUDIO_METER_IDENTITY_UNKNOWN);
	out_fill(0x0100, 0x0100);
	audio_scene_meter_output_period(out_period, TEST_FRAMES);
	read_output(0);
	check(snap.identity == SDK_AUDIO_METER_IDENTITY_UNKNOWN &&
	      snap.peak_hold_ch1 == 0x200U,
		"legacy playback meters under an unknown identity",
		fmt("identity=%lu peak=%08lx",
		    (unsigned long)snap.identity,
		    (unsigned long)snap.peak_hold_ch1));

	/* A participating AHI owner (it holds a submitted trim) is
	 * named while it drives the output. */
	check(audio_scene_trim_submit(AUDIO_SCENE_OWNER_AHI, -10, -10,
	                              NULL) == 0,
		"participating AHI trim accepted", NULL);
	read_output(0);
	check(snap.identity == SDK_AUDIO_METER_IDENTITY_AHI,
		"participating AHI owner is named", NULL);
	audio_scene_meter_output_identity(SDK_AUDIO_METER_IDENTITY_MEDIA);
	read_output(0);
	check(snap.identity == SDK_AUDIO_METER_IDENTITY_MEDIA,
		"pump binding outranks the AHI trim identity", NULL);
	audio_scene_meter_output_identity(
		SDK_AUDIO_METER_IDENTITY_UNKNOWN);
	audio_scene_trim_release(AUDIO_SCENE_OWNER_AHI);
	read_output(0);
	check(snap.identity == SDK_AUDIO_METER_IDENTITY_UNKNOWN,
		"trim release returns the identity to unknown", NULL);

	/* MHI participates through the media pump identity; its trim
	 * alone must not fabricate an identity. */
	check(audio_scene_trim_submit(AUDIO_SCENE_OWNER_MHI, -10, -10,
	                              NULL) == 0,
		"participating MHI trim accepted", NULL);
	read_output(0);
	check(snap.identity == SDK_AUDIO_METER_IDENTITY_UNKNOWN,
		"MHI trim alone stays unknown", NULL);
	audio_scene_trim_release(AUDIO_SCENE_OWNER_MHI);
}

/* ---- non-tearing snapshots under a concurrent writer (R9) ---- */

struct tear_ctx {
	volatile int done;
	uint32_t fed;
};

static void *tear_writer(void *arg)
{
	struct tear_ctx *ctx = arg;
	int16_t period[2 * 16];
	uint32_t v = 1;

	/* Both channels always carry the same magnitude, so every
	 * quiescent snapshot has peak_hold_ch1 == peak_hold_ch2; a torn
	 * read (fields from different writer updates) breaks equality. */
	while (!ctx->done && ctx->fed < 400000U) {
		uint32_t i;

		v = (v % 30000U) + 1U;
		for (i = 0; i < 16U; i++) {
			period[i * 2] = (int16_t)v;
			period[i * 2 + 1] = (int16_t)-(int)v;
		}
		audio_scene_meter_output_period(period, 16U);
		ctx->fed++;
	}
	ctx->done = 1;
	return NULL;
}

static void test_snapshot_no_tear(void)
{
	struct tear_ctx ctx;
	pthread_t writer;
	uint32_t reads = 0;
	uint32_t tears = 0;
	uint32_t nonzero = 0;

	audio_scene_init();
	memset(&ctx, 0, sizeof(ctx));
	check(pthread_create(&writer, NULL, tear_writer, &ctx) == 0,
		"writer thread starts", NULL);

	while (reads < 20000U) {
		struct audio_meter_snapshot s;

		if (audio_scene_meter_read(
			    SDK_AUDIO_METER_DIRECTION_OUTPUT, 0, &s) != 0) {
			tears++;  /* read failure is a broken contract */
			break;
		}
		if ((s.generation & 1U) != 0U ||
		    s.peak_hold_ch1 != s.peak_hold_ch2 ||
		    s.peak_hold_ch1 > 0x10000U) {
			tears++;
		}
		if (s.peak_hold_ch1 != 0U)
			nonzero++;
		reads++;
		if (ctx.done && reads > 1000U)
			break;
	}
	pthread_join(writer, NULL);

	check(tears == 0,
		"no torn snapshot under concurrent single-writer updates",
		fmt("tears=%lu reads=%lu", (unsigned long)tears,
		    (unsigned long)reads));
	check(reads > 1000U && nonzero > 0,
		"reader observed live accumulation",
		fmt("reads=%lu nonzero=%lu fed=%lu", (unsigned long)reads,
		    (unsigned long)nonzero, (unsigned long)ctx.fed));
}

/* ---- framed payload packing (KTD3 / SDK_OP_AUDIO_METER_READ) ---- */

static void test_frame_packing(void)
{
	struct SDKAudioMeterResultPayload payload;
	uint32_t gain_events;

	audio_scene_init();
	audio_scene_meter_output_identity(SDK_AUDIO_METER_IDENTITY_MEDIA);
	out_fill(0x1234, -0x2345);
	audio_scene_meter_output_period(out_period, TEST_FRAMES);
	audio_scene_meter_output_underrun();
	audio_scene_meter_output_underrun();

	/* A bounded trim emits exactly one gain-reduction event so the
	 * output snapshot carries a nonzero counter. */
	audio_scene_trim_submit(AUDIO_SCENE_OWNER_SDK, 60, 60, NULL);
	gain_events = audio_scene_gain_reduction_events();
	check(gain_events >= 1, "staging clamps for the event counter",
	      NULL);

	check(audio_scene_meter_read(SDK_AUDIO_METER_DIRECTION_OUTPUT,
	                             SDK_AUDIO_METER_RESULT_HOLD_RESET,
	                             &snap) == 0,
		"meter read succeeds for packing", NULL);
	memset(&payload, 0xa5, sizeof(payload));
	sdk_audio_meter_result_pack(&payload, &snap,
	                            SDK_AUDIO_METER_RESULT_HOLD_RESET);

	check(sizeof(payload) == 48U, "payload stays 48 bytes", NULL);
	check(w32(payload.direction) == SDK_AUDIO_METER_DIRECTION_OUTPUT,
		"frame packs direction", NULL);
	check(w32(payload.generation) == snap.generation,
		"frame packs the snapshot generation",
		fmt("packed=%08lx snap=%08lx",
		    (unsigned long)w32(payload.generation),
		    (unsigned long)snap.generation));
	check(w32(payload.frame) == 0U && w32(payload.frame_count) == 1U,
		"full state fits one 0-based frame", NULL);
	check(w32(payload.flags) == SDK_AUDIO_METER_RESULT_HOLD_RESET,
		"frame echoes the consumed-hold flag", NULL);
	check(w32(payload.identity) == snap.identity &&
	      w32(payload.identity) == SDK_AUDIO_METER_IDENTITY_MEDIA,
		"frame packs identity", NULL);
	check(w32(payload.clip_count) == snap.clip_count &&
	      w32(payload.underrun_count) == snap.underrun_count &&
	      w32(payload.underrun_count) == 2U &&
	      w32(payload.overrun_count) == snap.overrun_count,
		"frame packs saturating counters", NULL);
	check(w32(payload.gain_reduction_events) == gain_events,
		"frame packs gain-reduction events",
		fmt("packed=%08lx events=%lu",
		    (unsigned long)w32(payload.gain_reduction_events),
		    (unsigned long)gain_events));
	check(w32(payload.peak_hold_ch1) == 0x2468U &&
	      w32(payload.peak_hold_ch2) == 0x468aU,
		"frame packs both 16.16 peak holds",
		fmt("ch1=%08lx ch2=%08lx",
		    (unsigned long)w32(payload.peak_hold_ch1),
		    (unsigned long)w32(payload.peak_hold_ch2)));

	/* Opt-out reads pack without the consumed flag. */
	audio_scene_meter_read(SDK_AUDIO_METER_DIRECTION_OUTPUT, 0, &snap);
	sdk_audio_meter_result_pack(&payload, &snap, 0U);
	check(w32(payload.flags) == 0U &&
	      w32(payload.generation) == snap.generation,
		"peeking read packs a clean frame", NULL);

	/* Capture frames use the same packing. */
	audio_scene_meter_read(SDK_AUDIO_METER_DIRECTION_CAPTURE, 0, &snap);
	sdk_audio_meter_result_pack(&payload, &snap, 0U);
	check(w32(payload.direction) ==
	      SDK_AUDIO_METER_DIRECTION_CAPTURE &&
	      w32(payload.gain_reduction_events) == 0U,
		"capture frame packs its direction and no gain events", NULL);
}

/* ---- DSP re-init resets the meter instance (U2 discipline) ---- */

static void test_reset_with_dsp_reinit(void)
{
	audio_scene_init();
	audio_scene_meter_output_identity(SDK_AUDIO_METER_IDENTITY_MEDIA);
	out_fill(0x4000, 0x4000);
	audio_scene_meter_output_period(out_period, TEST_FRAMES);
	audio_scene_meter_output_underrun();
	audio_scene_meter_capture_overrun();
	cap_fill(INT16_MAX, 0);
	audio_scene_meter_capture_period(cap_period, TEST_FRAMES);
	check(audio_scene_apply_after_dsp_init() == 0,
		"dsp re-init re-apply succeeds", NULL);

	check(audio_scene_meter_read(SDK_AUDIO_METER_DIRECTION_OUTPUT,
	                             0, &snap) == 0 &&
	      snap.peak_hold_ch1 == 0 && snap.clip_count == 0 &&
	      snap.underrun_count == 0 &&
	      snap.identity == SDK_AUDIO_METER_IDENTITY_UNKNOWN &&
	      snap.gain_reduction_events == 0,
		"re-init resets output meters and identity",
		fmt("peak=%08lx clips=%08lx underruns=%08lx identity=%lu",
		    (unsigned long)snap.peak_hold_ch1,
		    (unsigned long)snap.clip_count,
		    (unsigned long)snap.underrun_count,
		    (unsigned long)snap.identity));
	check(audio_scene_meter_read(SDK_AUDIO_METER_DIRECTION_CAPTURE,
	                             0, &snap) == 0 &&
	      snap.peak_hold_ch1 == 0 && snap.clip_count == 0 &&
	      snap.overrun_count == 0,
		"re-init resets capture meters", NULL);

	/* Bounded full-size period: the firmware ISR scans exactly
	 * AUDIO_BYTES_PER_PERIOD/4 frames; prove the same bound here. */
	{
		static int16_t full[AUDIO_BYTES_PER_PERIOD / 2];

		memset(full, 0, sizeof(full));
		full[0] = 0x00ff;
		full[(AUDIO_BYTES_PER_PERIOD / 2) - 1] = -0x0100;
		audio_scene_meter_output_period(
			full, AUDIO_BYTES_PER_PERIOD / 4U);
		read_output(0);
		check(snap.peak_hold_ch1 == 0x1feU &&
		      snap.peak_hold_ch2 == 0x200U,
			"full 960-frame period scans to its exact bounds",
			fmt("ch1=%08lx ch2=%08lx",
			    (unsigned long)snap.peak_hold_ch1,
			    (unsigned long)snap.peak_hold_ch2));
	}
}

int main(void)
{
	test_peak_hold_consume_reset();
	test_clip_regions();
	test_capture_meter();
	test_counter_saturation();
	test_producer_stagnation();
	test_identity();
	test_snapshot_no_tear();
	test_frame_packing();
	test_reset_with_dsp_reinit();

	if (failures == 0) {
		printf("audio_meter_test: all tests passed\n");
		return 0;
	}
	printf("audio_meter_test: %d failure(s)\n", failures);
	return 1;
}
