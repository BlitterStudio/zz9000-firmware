/*
 * Host tests for the ZZ9000.CFG audio keys, boot wiring and the
 * persistence writer (plan U5): the audio control-plane key grammar
 * (KTD4) parses into scene state, corrupt or absent keys degrade to
 * the built-in defaults, the save regenerates the file from parsed
 * state plus live scene state through the temp-then-replace discipline
 * (KTD5), an injected mid-write failure leaves the original intact,
 * a scene above the enforced boundary is rejected before any I/O, and
 * the 4 KiB parse-budget truncation is observable through the query
 * key.
 *
 * Linked like audio_scene_test: the ax.h DSP setters are link-time
 * stubs, and zz_config.c's FatFs calls land in the in-memory mock
 * (fatfs_mock.c) so the atomicity logic runs for real on the host.
 */

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ff.h"
#include "audio_scene.h"
#include "ax.h"
#include "zz_config.h"

/* ---- link-time stubs for the ax.h DSP setters ---- */

int audio_adau_set_lpf_params(int f0) { (void)f0; return 0; }
int audio_adau_set_mixer_vol(int vol1, int vol2)
{
	(void)vol1;
	(void)vol2;
	return 0;
}
int audio_adau_set_prefactor(int pre) { (void)pre; return 0; }
int audio_adau_set_eq_gain(int band, int gain)
{
	(void)band;
	(void)gain;
	return 0;
}
int audio_adau_set_vol_pan(int vol, int pan)
{
	(void)vol;
	(void)pan;
	return 0;
}

int audio_adau_set_vol_pan_side(int side, int vol, int pan)
{
	(void)side; (void)vol; (void)pan; return 0;
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

/* ---- helpers ---- */

static void parse_str(const char *text)
{
	zz_config_reset();
	zz_config_parse(text, (unsigned)strlen(text));
}

static void scene_of(struct audio_scene_def *def, int lpf, int eq,
	int pref, int vol, int pan)
{
	memset(def, 0, sizeof(*def));
	def->lpf_hz = (uint16_t)lpf;
	for (int i = 0; i < AUDIO_SCENE_EQ_BANDS; i++)
		def->eq[i] = (uint8_t)eq;
	def->prefactor = (uint8_t)pref;
	def->volume = (uint8_t)vol;
	def->pan = (uint8_t)pan;
}

static void check_scene(int index, const struct audio_scene_def *want,
	const char *name)
{
	const struct audio_scene_def *got = audio_scene_get(index);

	for (int i = 0; i < AUDIO_SCENE_EQ_BANDS; i++) {
		check(got != NULL && got->eq[i] == want->eq[i],
			fmt("%s eq[%d]", name, i),
			fmt("got=%u want=%u", got ? got->eq[i] : 0,
				want->eq[i]));
	}
	check(got != NULL && got->lpf_hz == want->lpf_hz,
		fmt("%s lpf", name),
		fmt("got=%u want=%u", got ? got->lpf_hz : 0, want->lpf_hz));
	check(got != NULL && got->prefactor == want->prefactor,
		fmt("%s prefactor", name),
		fmt("got=%u want=%u", got ? got->prefactor : 0,
			want->prefactor));
	check(got != NULL && got->volume == want->volume,
		fmt("%s volume", name),
		fmt("got=%u want=%u", got ? got->volume : 0, want->volume));
	check(got != NULL && got->pan == want->pan,
		fmt("%s pan", name),
		fmt("got=%u want=%u", got ? got->pan : 0, want->pan));
	check(got != NULL &&
		strcmp(got->name, want->name) == 0,
		fmt("%s name", name),
		fmt("got='%s' want='%s'", got ? got->name : "?",
			want->name));
}

/* One complete audio block: every one of the 130 audio keys (66
 * parameter keys plus the 64 name-chunk keys), each with a value
 * distinct from the built-in defaults. */
static void parse_full_audio_block(void)
{
	char text[4096];
	int off = 0;

	off += snprintf(text + off, sizeof(text) - off,
		"audio_active = 5\n"
		"audio_baseline = %u\n", 170u * 256u + 130u);
	for (int i = 0; i < AUDIO_SCENE_COUNT; i++) {
		/* Scene 0 carries hand-picked packed values; the others a
		 * uniform (100, 0) pair shape. Every scene is named
		 * "CfgTest<N>" as four character chunks plus terminator
		 * and zero padding. */
		unsigned lpf = i == 0 ? 12000u : 8000u + 2000u * (unsigned)i;
		unsigned eq0 = i == 0 ? 12800u : 12800u;
		unsigned eq1 = i == 0 ? 6450u : 12800u;
		unsigned eq2 = i == 0 ? 12900u : 12800u;
		unsigned eq3 = i == 0 ? 100u : 12800u;
		unsigned eq4 = i == 0 ? 51u : 12800u;
		unsigned out = 12900u;
		unsigned pan = i == 0 ? 75u : 40u + (unsigned)i;

		off += snprintf(text + off, sizeof(text) - off,
			"audio_scene%d_lpf = %u\n"
			"audio_scene%d_eq01 = %u\n"
			"audio_scene%d_eq23 = %u\n"
			"audio_scene%d_eq45 = %u\n"
			"audio_scene%d_eq67 = %u\n"
			"audio_scene%d_eq89 = %u\n"
			"audio_scene%d_out = %u\n"
			"audio_scene%d_pan = %u\n"
			"audio_scene%d_nm1 = %u\n"
			"audio_scene%d_nm2 = %u\n"
			"audio_scene%d_nm3 = %u\n"
			"audio_scene%d_nm4 = %u\n"
			"audio_scene%d_nm5 = 0\n"
			"audio_scene%d_nm6 = 0\n"
			"audio_scene%d_nm7 = 0\n"
			"audio_scene%d_nm8 = 0\n",
			i, lpf, i, eq0, i, eq1, i, eq2, i, eq3, i, eq4,
			i, out, i, pan,
			i, 17254u, i, 26452u, i, 25971u, i, 29744u + (unsigned)i,
			i, i, i, i);
	}
	parse_str(text);
}

/* ---- parse: every audio key round-trips into scene state ---- */

static void test_parse_all_audio_keys(void)
{
	const struct zz_config *c;
	struct audio_scene_def want;

	audio_scene_init();
	parse_full_audio_block();
	audio_scene_load_config();

	c = zz_config_get();
	check(c->audio_active == 5 && c->audio_active_present,
		"audio_active parsed", fmt("v=%u", c->audio_active));
	check(c->audio_baseline == 170u * 256u + 130u &&
		c->audio_baseline_present,
		"audio_baseline parsed", fmt("v=%u", c->audio_baseline));
	for (int i = 0; i < AUDIO_SCENE_COUNT; i++)
		check(c->audio_scene_mask[i] == 0xffffu,
			fmt("scene %d key mask complete", i),
			fmt("mask=0x%x", c->audio_scene_mask[i]));

	check(audio_scene_active_index() == 5, "active scene applied",
		fmt("active=%u", audio_scene_active_index()));
	check(audio_scene_baseline_paula() == 170 &&
		audio_scene_baseline_ax() == 130,
		"baseline applied",
		fmt("paula=%u ax=%u", audio_scene_baseline_paula(),
			audio_scene_baseline_ax()));

	/* Scene 0 carries hand-picked packed values; the packed decode is
	 * the point of the check. */
	memset(&want, 0, sizeof(want));
	want.lpf_hz = 12000;
	want.eq[0] = 100; want.eq[1] = 0;    /* eq01 = 12800 */
	want.eq[2] = 50;  want.eq[3] = 50;   /* eq23 = 6450 */
	want.eq[4] = 100; want.eq[5] = 100;  /* eq45 = 12900 */
	want.eq[6] = 0;   want.eq[7] = 100;  /* eq67 = 100 */
	want.eq[8] = 0;   want.eq[9] = 51;   /* eq89 = 51 */
	want.prefactor = 100;                /* out = 12900 */
	want.volume = 100;
	want.pan = 75;
	strcpy(want.name, "CfgTest0");
	check_scene(0, &want, "scene 0");

	/* Scenes 1..7 use one uniform shape with per-scene values. */
	for (int i = 1; i < AUDIO_SCENE_COUNT; i++) {
		scene_of(&want, 8000 + 2000 * i, 50, 100, 100, 40 + i);
		/* eq01..eq89 = 12800 packs (100, 0) for these scenes. */
		for (int k = 0; k < AUDIO_SCENE_EQ_BANDS; k += 2) {
			want.eq[k] = 100;
			want.eq[k + 1] = 0;
		}
		snprintf(want.name, sizeof(want.name), "CfgTest%d", i);
		check_scene(i, &want, fmt("scene %d", i));
	}
}

static void test_absent_and_corrupt_degrade(void)
{
	struct audio_scene_def defaults[AUDIO_SCENE_COUNT];

	audio_scene_init();
	for (int i = 0; i < AUDIO_SCENE_COUNT; i++)
		defaults[i] = *audio_scene_get(i);

	/* Absent: only a valid audio_active line; nothing else changes. */
	parse_str("audio_active = 3\n");
	audio_scene_load_config();
	check(audio_scene_active_index() == 3, "absent keys keep defaults",
		fmt("active=%u", audio_scene_active_index()));
	for (int i = 0; i < AUDIO_SCENE_COUNT; i++)
		check_scene(i, &defaults[i], fmt("default scene %d", i));

	/* Corrupt: every value below is out of range or garbage, so the
	 * whole file degrades to defaults without an error path. */
	parse_str(
		"audio_active = 8\n"
		"audio_active = xyz\n"
		"audio_baseline = 70000\n"
		"audio_scene2_lpf = 0\n"
		"audio_scene2_lpf = 23901\n"
		"audio_scene2_eq01 = 13056\n"   /* hi band 102 > 100 */
		"audio_scene2_eq45 = 12928\n"   /* hi band 101 > 100 */
		"audio_scene2_out = 101\n"      /* lo 101 > 100 */
		"audio_scene2_pan = 101\n"
		"audio_scene4_eq23 = -3\n"
		"audio_scene6_lpf\n"
		"audio_scene5_nm1 = 1\n"       /* NUL first char, not a
					       * pure terminator */
		"audio_scene5_nm2 = 2625\n"    /* 0x0a41: control char */
		"audio_scene5_nm3 = 70000\n"
		"audio_scene3_nm1 = 0\n"       /* valid terminator only: an
					       * explicitly emptied name */
		"audio_scene1_nm2 = 8256\n");  /* valid: ' '@0x20,'@' */
	audio_scene_init();
	audio_scene_load_config();
	check(audio_scene_active_index() == 0,
		"corrupt audio_active rejected",
		fmt("active=%u", audio_scene_active_index()));
	check(audio_scene_baseline_paula() == 128 &&
		audio_scene_baseline_ax() == 64,
		"corrupt audio_baseline rejected",
		fmt("paula=%u ax=%u", audio_scene_baseline_paula(),
			audio_scene_baseline_ax()));
	for (int i = 0; i < AUDIO_SCENE_COUNT; i++)
		check_scene(i, &defaults[i], fmt("degraded scene %d", i));
	check(zz_config_get()->audio_scene_mask[2] == 0 &&
		zz_config_get()->audio_scene_mask[4] == 0 &&
		zz_config_get()->audio_scene_mask[5] == 0,
		"corrupt scene keys leave no mask bits", NULL);
	check(strcmp(audio_scene_get(3)->name, "Scene 4") == 0,
		"an explicitly emptied name falls back to the default label",
		audio_scene_get(3)->name);
	check(strcmp(audio_scene_get(1)->name, "Scene 2") == 0,
		"a name group without a leading chunk falls back too",
		audio_scene_get(1)->name);
}

/* ---- save round-trip: the saved file reparses identically ---- */

static void test_save_roundtrip(void)
{
	static struct audio_scene_def written[AUDIO_SCENE_COUNT];
	const char *saved;
	uint16_t present = 0;
	int len;

	mock_fs_reset();
	parse_str(
		"videocap_profile = centered_1080p_60\n"
		"videocap_sample = odd\n"
		"videocap_crop_h = 300\n"
		"videocap_crop_v = 31\n"
		"scanline_mode = 3\n"
		"scanline_parity = 1\n"
		"int2 = on\n"
		"offscreen_bitmaps = off\n"
		"video_overlay = off\n"
		"mac = 68:82:F2:12:34:56\n"
		"hdf = games.hdf\n");

	audio_scene_init();
	for (int i = 0; i < AUDIO_SCENE_COUNT; i++) {
		/* Scene 0 composes below the boundary even with the raised
		 * baseline below, and since the save validates every slot
		 * (R15 all-slot rule), scenes 1..7 are kept within it too. */
		if (i == 0)
			scene_of(&written[0], 23900, 50, 50, 90, 50);
		else
			scene_of(&written[i], 9000 + 1000 * i, 55, 60, 50,
				30 + i);
		/* Every scene carries a saved name: "Slot N" as three
		 * chunks plus a terminator character. */
		snprintf(written[i].name, sizeof(written[i].name),
			"Slot %d", i);
		check(audio_scene_write(i, &written[i]) == 0,
			fmt("scene %d written", i), NULL);
	}
	check(audio_scene_select(4) == 0, "active scene selected", NULL);
	check(audio_scene_set_baseline(140, 70) == 0, "baseline set", NULL);

	check(audio_scene_save(0) == AUDIO_SCENE_SAVE_OK, "save succeeds",
		NULL);

	saved = mock_fs_file("0:/ZZ9000.CFG");
	len = mock_fs_file_len("0:/ZZ9000.CFG");
	check(saved != NULL && len > 0, "file written", fmt("len=%d", len));
	if (!saved)
		return;
	/* The name keys ride along in the emitted file. */
	check(strstr(saved, "audio_scene0_nm1 = ") != NULL &&
		strstr(saved, "audio_scene0_nm4 = ") != NULL,
		"save includes the name keys", NULL);
	/* The saved text must reparse into the same state. */
	zz_config_reset();
	check(zz_config_parse(saved, (unsigned)len) == 141,
		"every key line accepted on reparse", NULL);
	audio_scene_init();
	audio_scene_load_config();

	const struct zz_config *c = zz_config_get();
	check(c->videocap_output_profile ==
		ZZ_VIDEOCAP_OUTPUT_CENTERED_1080P_60 &&
		c->videocap_mode_present && c->videocap_shres_present &&
		c->ns_vsync_present,
		"videocap profile round-trips", NULL);
	check(c->videocap_sample == 2 && c->videocap_sample_present,
		"videocap_sample round-trips", NULL);
	check(c->videocap_crop_h == 300 && c->videocap_crop_h_present &&
		c->videocap_crop_v == 31 && c->videocap_crop_v_present,
		"crop round-trips", NULL);
	check(c->scanline_mode == 3 && c->scanline_mode_present &&
		c->scanline_parity == 1 && c->scanline_parity_present,
		"scanlines round-trip", NULL);
	check(c->int2 == 1 && c->int2_present, "int2 round-trips", NULL);
	check(c->offscreen_bitmaps == 0 && c->offscreen_bitmaps_present &&
		c->video_overlay == 0 && c->video_overlay_present,
		"feature switches round-trip", NULL);
	check(c->mac_present && c->mac[0] == 0x68 && c->mac[5] == 0x56,
		"mac round-trips", NULL);
	check(c->hdf_present &&
		strcmp(c->hdf_path, "0:/games.hdf") == 0,
		"hdf round-trips", c->hdf_path);

	check(audio_scene_active_index() == 4, "active round-trips",
		fmt("active=%u", audio_scene_active_index()));
	check(audio_scene_baseline_paula() == 140 &&
		audio_scene_baseline_ax() == 70,
		"baseline round-trips", NULL);
	for (int i = 0; i < AUDIO_SCENE_COUNT; i++)
		check_scene(i, &written[i], fmt("round-trip scene %d", i));

	(void)present;
}

/* Every videocap_profile name must regenerate canonically. */
static void test_save_regenerates_every_profile_name(void)
{
	static const char *names[] = {
		"full_60", "full_exact", "filtered_60", "filtered_pal",
		"filtered_pal_exact", "filtered_ntsc_exact",
		"centered_1080p_60"
	};
	char line[64];

	for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		const char *saved;

		snprintf(line, sizeof(line), "videocap_profile = %s\n",
			names[i]);
		parse_str(line);
		audio_scene_init();
		mock_fs_reset();
		check(audio_scene_save(0) == AUDIO_SCENE_SAVE_OK,
			fmt("save with profile %s", names[i]), NULL);
		saved = mock_fs_file("0:/ZZ9000.CFG");
		snprintf(line, sizeof(line), "videocap_profile = %s\n",
			names[i]);
		check(saved != NULL && strstr(saved, line) != NULL,
			fmt("profile %s regenerates canonically", names[i]),
			saved ? saved : "(none)");
	}
}

/* ---- budget: 8 scenes + all documented keys fit the 4 KiB cap ---- */

static void test_save_budget(void)
{
	struct audio_scene_def wide;
	const char *saved;
	int len;

	mock_fs_reset();
	parse_str(
		"videocap_profile = centered_1080p_60\n"
		"videocap_sample = odd\n"
		"videocap_crop_h = 4095\n"
		"videocap_crop_v = 4095\n"
		"scanline_mode = 3\n"
		"scanline_parity = 1\n"
		"int2 = on\n"
		"offscreen_bitmaps = off\n"
		"video_overlay = off\n"
		"mac = 68:82:F2:12:34:56\n"
		"hdf = maximum-length.hdf\n");
	audio_scene_init();
	/* Scenes 1..7 serialize at wide boundary-safe values (five-digit
	 * lpf, four-digit packed pairs, three-digit pan) and a full
	 * 16-character name (five-digit name chunks), so the budget sees
	 * the largest possible audio block. The all-slot save validation
	 * (R15) rejects any over-boundary scene, so the range-widest
	 * eq/prefactor/volume 100 shape can never reach the writer
	 * anymore. */
	scene_of(&wide, 23900, 60, 50, 50, 100);
	strcpy(wide.name, "WidestLegalScene");
	for (int i = 1; i < AUDIO_SCENE_COUNT; i++)
		check(audio_scene_write(i, &wide) == 0,
			fmt("wide scene %d written", i), NULL);
	check(audio_scene_select(0) == 0, "scene 0 active", NULL);

	check(audio_scene_save(0) == AUDIO_SCENE_SAVE_OK,
		"widest legal state saves", NULL);
	len = mock_fs_file_len("0:/ZZ9000.CFG");
	saved = mock_fs_file("0:/ZZ9000.CFG");
	check(len > 0 && (unsigned)len < ZZ_CONFIG_MAX_SIZE,
		"serialized size fits the parse budget",
		fmt("len=%d cap=%u", len, (unsigned)ZZ_CONFIG_MAX_SIZE));
	printf("audio_config: serialized 8 scenes + all keys = %d bytes "
	       "(cap %u)\n", len, (unsigned)ZZ_CONFIG_MAX_SIZE);

	zz_config_reset();
	check(saved != NULL &&
		zz_config_parse(saved, (unsigned)len) == 141,
		"widest file reparses with every key accepted", NULL);
}

/* ---- atomicity: injected mid-write failure leaves the original ---- */

static void test_save_midwrite_failure(void)
{
	mock_fs_reset();
	mock_fs_set_file("0:/ZZ9000.CFG", "scanline_mode = 2\n");
	parse_str("scanline_mode = 2\n");
	audio_scene_init();

	mock_fail_write(1);
	check(audio_scene_save(0) == AUDIO_SCENE_SAVE_IO_ERROR,
		"mid-write failure surfaces IO error", NULL);
	check(mock_fs_file("0:/ZZ9000.CFG") != NULL &&
		strcmp(mock_fs_file("0:/ZZ9000.CFG"), "scanline_mode = 2\n")
			== 0,
		"original file intact after mid-write failure", NULL);
	check(mock_fs_file("0:/ZZCFG.TMP") != NULL,
		"partial temp file left for the reset hook", NULL);

	/* The Amiga-reset hook drops the junk temp (fw_update_reset's
	 * CFG counterpart); the original is untouched either way. */
	zz_config_save_reset();
	check(mock_fs_file("0:/ZZCFG.TMP") == NULL,
		"reset hook drops the partial temp", NULL);
	check(mock_fs_file("0:/ZZ9000.CFG") != NULL &&
		strcmp(mock_fs_file("0:/ZZ9000.CFG"), "scanline_mode = 2\n")
			== 0,
		"original still intact after reset hook", NULL);

	/* The next save cleans its own stale temp and succeeds. */
	mock_fail_write(0);
	check(audio_scene_save(0) == AUDIO_SCENE_SAVE_OK,
		"next save recovers", NULL);
	check(mock_fs_file("0:/ZZ9000.CFG") != NULL &&
		strstr(mock_fs_file("0:/ZZ9000.CFG"),
			"scanline_mode = 2\n") != NULL,
		"regenerated file keeps the parsed non-audio keys", NULL);
	check(mock_fs_file("0:/ZZCFG.TMP") == NULL,
		"successful save leaves no temp", NULL);
	check(mock_fs_file("0:/ZZ9000.BAK") != NULL &&
		strcmp(mock_fs_file("0:/ZZ9000.BAK"),
			"scanline_mode = 2\n") == 0,
		"previous file kept as ZZ9000.BAK", NULL);
}

static void test_save_rename_failure_restores_original(void)
{
	/* Commit rename (temp -> ZZ9000.CFG) fails: the original was
	 * already moved to ZZ9000.BAK and must come back. */
	mock_fs_reset();
	mock_fs_set_file("0:/ZZ9000.CFG", "int2 = on\n");
	parse_str("int2 = on\n");
	audio_scene_init();
	mock_fail_rename(2);
	check(audio_scene_save(0) == AUDIO_SCENE_SAVE_IO_ERROR,
		"commit rename failure surfaces IO error", NULL);
	check(mock_fs_file("0:/ZZ9000.CFG") != NULL &&
		strcmp(mock_fs_file("0:/ZZ9000.CFG"), "int2 = on\n") == 0,
		"original restored after commit rename failure", NULL);

	/* Backup rename fails first: nothing was touched yet. */
	mock_fs_reset();
	mock_fs_set_file("0:/ZZ9000.CFG", "int2 = on\n");
	parse_str("int2 = on\n");
	audio_scene_init();
	mock_fail_rename(1);
	check(audio_scene_save(0) == AUDIO_SCENE_SAVE_IO_ERROR,
		"backup rename failure surfaces IO error", NULL);
	check(mock_fs_file("0:/ZZ9000.CFG") != NULL &&
		strcmp(mock_fs_file("0:/ZZ9000.CFG"), "int2 = on\n") == 0,
		"original untouched after backup rename failure", NULL);
}

/* ---- validation: over-boundary scene never reaches the writer ---- */

static void test_save_rejects_over_boundary(void)
{
	struct audio_scene_def loud;

	mock_fs_reset();
	audio_scene_init();
	scene_of(&loud, 23900, 50, 100, 100, 50); /* +12 dB master boost */
	check(audio_scene_write(1, &loud) == 0, "loud scene stored", NULL);

	check(audio_scene_save(1) == AUDIO_SCENE_SAVE_REJECTED,
		"over-boundary scene rejected at save", NULL);
	check(mock_fs_file("0:/ZZ9000.CFG") == NULL &&
		mock_fs_file("0:/ZZCFG.TMP") == NULL,
		"rejected save writes nothing", NULL);
	check(audio_scene_save(200) == -1, "invalid scene index rejected",
		NULL);
	check(audio_scene_save(0) == AUDIO_SCENE_SAVE_REJECTED,
		"cross-slot: an over-boundary scene rejects every save",
		NULL);
	check(mock_fs_file("0:/ZZ9000.CFG") == NULL &&
		mock_fs_file("0:/ZZCFG.TMP") == NULL,
		"cross-slot rejection still writes nothing", NULL);

	/* Back within bounds everywhere: the save goes through. */
	audio_scene_init();
	check(audio_scene_save(0) == AUDIO_SCENE_SAVE_OK,
		"save proceeds once every slot is within bounds", NULL);
	check(mock_fs_file("0:/ZZ9000.CFG") != NULL,
		"recovered save persists the CFG", NULL);
}

/* ---- truncation: the 4 KiB overflow is queryable ---- */

static void test_truncation_query_key(void)
{
	static char big[4400];
	uint16_t present = 0;
	unsigned off = 0;

	/* A file past the parse budget with audio keys in the ignored
	 * tail. */
	while (off < 4160) {
		big[off++] = '#';
		for (int i = 0; i < 78; i++)
			big[off++] = 'x';
		big[off++] = '\n';
	}
	memcpy(big + off, "audio_active = 7\n", 17);
	off += 17;

	mock_fs_reset();
	mock_fs_set_file("0:/ZZ9000.CFG", big);
	check(zz_config_load() == 0, "oversized file still loads", NULL);
	check(zz_config_query(ZZ_CONFIG_KEY_AUDIO_TRUNCATED, &present) == 1 &&
		present == 1,
		"truncation query key reports the ignored tail",
		fmt("v=%u present=%u",
			zz_config_query(ZZ_CONFIG_KEY_AUDIO_TRUNCATED, NULL),
			present));

	audio_scene_init();
	audio_scene_load_config();
	check(audio_scene_active_index() == 0,
		"truncated audio keys do not apply",
		fmt("active=%u", audio_scene_active_index()));

	mock_fs_reset();
	mock_fs_set_file("0:/ZZ9000.CFG", "videocap_sample = even\n");
	check(zz_config_load() == 0, "small file loads", NULL);
	check(zz_config_query(ZZ_CONFIG_KEY_AUDIO_TRUNCATED, &present) == 0 &&
		present == 1,
		"small file reports no truncation",
		fmt("v=%u present=%u",
			zz_config_query(ZZ_CONFIG_KEY_AUDIO_TRUNCATED, NULL),
			present));

	mock_fs_reset();
	zz_config_reset();
	zz_config_load();
	present = 1;
	(void)zz_config_query(ZZ_CONFIG_KEY_AUDIO_TRUNCATED, &present);
	check(present == 0, "no file: truncation key absent", NULL);
}

/* The shipped sample's commented audio block must carry exactly the
 * packed firmware defaults, so uncommenting any line reproduces the
 * power-on state (sample numbers parse AND match). */
static void check_sample_audio_defaults(const char *sample)
{
	static const char *eq_pair_names[5] = { "01", "23", "45", "67",
		"89" };
	char line[48];
	int i, k;

	audio_scene_init();
	snprintf(line, sizeof(line), "audio_active = %u",
		(unsigned)audio_scene_active_index());
	check(strstr(sample, line) != NULL, "sample pins audio_active",
		line);
	snprintf(line, sizeof(line), "audio_baseline = %u",
		(unsigned)audio_scene_baseline_paula() * 256u +
		(unsigned)audio_scene_baseline_ax());
	check(strstr(sample, line) != NULL, "sample pins audio_baseline",
		line);
	for (i = 0; i < AUDIO_SCENE_COUNT; i++) {
		const struct audio_scene_def *s = audio_scene_get(i);

		snprintf(line, sizeof(line), "audio_scene%d_lpf = %u",
			i, (unsigned)s->lpf_hz);
		check(strstr(sample, line) != NULL,
			"sample pins scene lpf default", line);
		for (k = 0; k < 5; k++) {
			snprintf(line, sizeof(line),
				"audio_scene%d_eq%s = %u", i,
				eq_pair_names[k],
				(unsigned)s->eq[2 * k] * 128u +
				(unsigned)s->eq[2 * k + 1]);
			check(strstr(sample, line) != NULL,
				"sample pins scene eq pair default", line);
		}
		snprintf(line, sizeof(line), "audio_scene%d_out = %u",
			i, (unsigned)s->prefactor * 128u +
			(unsigned)s->volume);
		check(strstr(sample, line) != NULL,
			"sample pins scene out pair default", line);
		snprintf(line, sizeof(line), "audio_scene%d_pan = %u",
			i, (unsigned)s->pan);
		check(strstr(sample, line) != NULL,
			"sample pins scene pan default", line);
		/* The sample documents the name grammar once (scene 0's
		 * group, leading chunks); pin those to the default label
		 * "Scene 1". */
		if (i == 0) {
			for (k = 0; k < 4; k++) {
				snprintf(line, sizeof(line),
					"audio_scene%d_nm%d = %u", i, k + 1,
					(unsigned)(uint8_t)s->name[2 * k] *
					256u +
					(unsigned)(uint8_t)s->name[2 * k + 1]);
				check(strstr(sample, line) != NULL,
					"sample pins scene name chunk default",
					line);
			}
		}
	}
}

/* The shipped sample must fit the parse budget so every documented
 * key stays loadable (the restructure under the 4 KiB cap). */
static void test_sample_file_parses_fully(void)
{
	static char sample[8192];
	FILE *f = fopen("../../ZZ9000.CFG", "rb");
	size_t n;
	uint16_t present = 0;

	check(f != NULL, "shipped ZZ9000.CFG sample present", NULL);
	if (!f)
		return;
	n = fread(sample, 1, sizeof(sample) - 1, f);
	fclose(f);
	sample[n] = 0;
	check((unsigned)n < ZZ_CONFIG_MAX_SIZE,
		"sample fits the parse budget",
		fmt("len=%u cap=%u", (unsigned)n,
			(unsigned)ZZ_CONFIG_MAX_SIZE));

	mock_fs_reset();
	mock_fs_set_file("0:/ZZ9000.CFG", sample);
	check(zz_config_load() == 0, "sample loads", NULL);
	check(zz_config_query(ZZ_CONFIG_KEY_AUDIO_TRUNCATED, &present) == 0 &&
		present == 1,
		"sample loads without truncation", NULL);

	/* Every documented audio default in the sample must equal the
	 * packed firmware default: the numbers must not only parse but
	 * match what a fresh audio_scene_init() would emit. */
	check_sample_audio_defaults(sample);
}

int main(void)
{
	mock_fs_reset();
	test_parse_all_audio_keys();
	test_absent_and_corrupt_degrade();
	test_save_roundtrip();
	test_save_regenerates_every_profile_name();
	test_save_budget();
	test_save_midwrite_failure();
	test_save_rename_failure_restores_original();
	test_save_rejects_over_boundary();
	test_truncation_query_key();
	test_sample_file_parses_fully();

	if (failures) {
		printf("%d failure(s)\n", failures);
		return 1;
	}
	printf("audio_config: all checks passed\n");
	return 0;
}

