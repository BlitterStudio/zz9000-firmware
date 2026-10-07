/* Behavioural tests for preferences, ahiprefs, and backend resolution.
 * SPDX-License-Identifier: GPL-3.0-or-later */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../tools/zzplay/zzplay-ahiprefs.h"
#include "../tools/zzplay/zzplay-options.h"
#include "../tools/zzplay/zzplay-prefs.h"

static int test_prefs_parse_and_format(void)
{
  static const char prefs_text[] =
      "# ZZPlay settings\n"
      "MP3OUTPUT = MHI\r\n"
      "VIDEOAUDIO=AX\n"
      "AHIUNIT= 2 \n"
      "MHIDRIVER=mycustom.library\n"
      "VOLUME=75\n"
      "REPEAT=ONE\n"
      "SHUFFLE=YES\n"
      "WINDOW=120,45\n"
      "DRAWER=DH0:Music/MP3\n";

  ZZPlayPrefs prefs;
  char out_buf[1024];
  size_t needed;
  uint32_t accepted;

  zzplay_prefs_defaults(&prefs);
  accepted = zzplay_prefs_parse(&prefs, prefs_text, strlen(prefs_text));
  if (accepted != 9U) return 1;

  if (prefs.mp3_output != ZZPLAY_AUDIO_MHI) return 2;
  if (prefs.video_audio != ZZPLAY_AUDIO_AX) return 3;
  if (prefs.ahi_unit != 2U) return 4;
  if (strcmp(prefs.mhi_driver, "mycustom.library") != 0) return 5;
  if (prefs.volume != 75U) return 6;
  if (prefs.repeat != ZZPLAY_REPEAT_ONE) return 7;
  if (!prefs.shuffle) return 8;
  if (prefs.window_left != 120 || prefs.window_top != 45) return 9;
  if (strcmp(prefs.last_drawer, "DH0:Music/MP3") != 0) return 10;

  /* Format */
  needed = zzplay_prefs_format(&prefs, out_buf, sizeof(out_buf));
  if (needed == 0U) return 11;
  if (strstr(out_buf, "MP3OUTPUT=MHI\n") == NULL) return 12;
  if (strstr(out_buf, "VIDEOAUDIO=AX\n") == NULL) return 13;
  if (strstr(out_buf, "AHIUNIT=2\n") == NULL) return 14;
  if (strstr(out_buf, "MHIDRIVER=mycustom.library\n") == NULL) return 15;
  if (strstr(out_buf, "VOLUME=75\n") == NULL) return 16;
  if (strstr(out_buf, "REPEAT=ONE\n") == NULL) return 17;
  if (strstr(out_buf, "SHUFFLE=YES\n") == NULL) return 18;
  if (strstr(out_buf, "WINDOW=120,45\n") == NULL) return 19;
  if (strstr(out_buf, "DRAWER=DH0:Music/MP3\n") == NULL) return 20;

  /* Round trip */
  {
    ZZPlayPrefs roundtrip;
    zzplay_prefs_defaults(&roundtrip);
    if (zzplay_prefs_parse(&roundtrip, out_buf, strlen(out_buf)) != 9U)
      return 21;
    if (roundtrip.mp3_output != ZZPLAY_AUDIO_MHI) return 22;
    if (roundtrip.video_audio != ZZPLAY_AUDIO_AX) return 23;
    if (roundtrip.ahi_unit != 2U) return 24;
    if (roundtrip.volume != 75U) return 25;
    if (roundtrip.repeat != ZZPLAY_REPEAT_ONE) return 26;
  }

  return 0;
}

static int test_prefs_bad_values(void)
{
  ZZPlayPrefs prefs;

  zzplay_prefs_defaults(&prefs);

  /* MP3OUTPUT cannot be AX -> keeps previous default (AUTO) */
  zzplay_prefs_parse(&prefs, "MP3OUTPUT=AX\n", 13U);
  if (prefs.mp3_output != ZZPLAY_AUDIO_AUTO) return 1;

  /* VIDEOAUDIO cannot be MHI -> keeps default (AUTO) */
  zzplay_prefs_parse(&prefs, "VIDEOAUDIO=MHI\n", 15U);
  if (prefs.video_audio != ZZPLAY_AUDIO_AUTO) return 2;

  /* AHIUNIT must be 0..3 */
  zzplay_prefs_parse(&prefs, "AHIUNIT=4\n", 10U);
  if (prefs.ahi_unit != 0U) return 3;
  zzplay_prefs_parse(&prefs, "AHIUNIT=-1\n", 11U);
  if (prefs.ahi_unit != 0U) return 4;

  /* MHIDRIVER cannot have colons or slashes, or be empty */
  zzplay_prefs_parse(&prefs, "MHIDRIVER=LIBS:MHI/bad.library\n", 31U);
  if (strcmp(prefs.mhi_driver, ZZPLAY_PREFS_DEFAULT_MHI_DRIVER) != 0) return 5;
  zzplay_prefs_parse(&prefs, "MHIDRIVER=\n", 11U);
  if (strcmp(prefs.mhi_driver, ZZPLAY_PREFS_DEFAULT_MHI_DRIVER) != 0) return 6;

  /* VOLUME must be 0..100 */
  zzplay_prefs_parse(&prefs, "VOLUME=150\n", 11U);
  if (prefs.volume != 100U) return 7;
  zzplay_prefs_parse(&prefs, "VOLUME=-5\n", 10U);
  if (prefs.volume != 100U) return 8;

  /* Valid driver validation helper */
  if (zzplay_prefs_valid_driver("mhizz9000.library") != 1) return 9;
  if (zzplay_prefs_valid_driver("LIBS:mhizz9000.library") != 0) return 10;
  if (zzplay_prefs_valid_driver("dir/mhizz9000.library") != 0) return 11;
  if (zzplay_prefs_valid_driver("") != 0) return 12;

  return 0;
}

static int test_session_read_modify_write(void)
{
  const char *tmp_path = "T_zzplay_test_prefs.tmp";
  ZZPlayPrefs initial;
  ZZPlayPrefs session;
  ZZPlayPrefs verified;

  /* Setup initial file with custom device settings */
  zzplay_prefs_defaults(&initial);
  initial.ahi_unit = 3U;
  strcpy(initial.mhi_driver, "custom.library");
  initial.mp3_output = ZZPLAY_AUDIO_MHI;
  initial.video_audio = ZZPLAY_AUDIO_AX;
  initial.volume = 50U;
  if (!zzplay_prefs_save(&initial, tmp_path)) return 1;

  /* Session has modified session-only fields */
  zzplay_prefs_defaults(&session);
  session.volume = 85U;
  session.repeat = ZZPLAY_REPEAT_ALL;
  session.shuffle = 1;
  session.window_left = 200;
  session.window_top = 150;
  strcpy(session.last_drawer, "RAM:T");

  /* Device settings in 'session' differ from saved (e.g. defaults) */
  session.ahi_unit = 0U;
  strcpy(session.mhi_driver, "mhizz9000.library");
  session.mp3_output = ZZPLAY_AUDIO_AUTO;

  /* Save session into tmp_path */
  if (!zzplay_prefs_save_session(&session, tmp_path)) {
    remove(tmp_path);
    return 2;
  }

  /* Load back: device settings must be preserved from initial, session updated */
  if (!zzplay_prefs_load(&verified, tmp_path)) {
    remove(tmp_path);
    return 3;
  }
  remove(tmp_path);

  /* Device settings preserved */
  if (verified.ahi_unit != 3U) return 4;
  if (strcmp(verified.mhi_driver, "custom.library") != 0) return 5;
  if (verified.mp3_output != ZZPLAY_AUDIO_MHI) return 6;
  if (verified.video_audio != ZZPLAY_AUDIO_AX) return 7;

  /* Session settings updated */
  if (verified.volume != 85U) return 8;
  if (verified.repeat != ZZPLAY_REPEAT_ALL) return 9;
  if (!verified.shuffle) return 10;
  if (verified.window_left != 200 || verified.window_top != 150) return 11;
  if (strcmp(verified.last_drawer, "RAM:T") != 0) return 12;

  return 0;
}

static int test_session_settle_drops_launch_overrides(void)
{
  ZZPlayPrefs stored;
  ZZPlayPrefs started;
  ZZPlayPrefs session;

  /* Saved: volume 70, repeat off. This launch: VOLUME=20 and LOOP. */
  zzplay_prefs_defaults(&stored);
  stored.volume = 70U;
  stored.repeat = ZZPLAY_REPEAT_OFF;
  stored.shuffle = 1;
  started = stored;
  started.volume = 20U;
  started.repeat = ZZPLAY_REPEAT_ONE;

  /* Untouched overrides fall back to the saved values. */
  session = started;
  zzplay_prefs_settle_session(&session, &stored, &started);
  if (session.volume != 70U) return 1;
  if (session.repeat != ZZPLAY_REPEAT_OFF) return 2;
  if (!session.shuffle) return 3;

  /* Values the user changed during the session are remembered. */
  session = started;
  session.volume = 45U;
  session.repeat = ZZPLAY_REPEAT_ALL;
  session.shuffle = 0;
  zzplay_prefs_settle_session(&session, &stored, &started);
  if (session.volume != 45U) return 4;
  if (session.repeat != ZZPLAY_REPEAT_ALL) return 5;
  if (session.shuffle) return 6;

  return 0;
}

static int test_requested_backend_matrix(void)
{
  ZZPlayPrefs prefs;
  ZZPlayOptions options;
  int strict = -1;
  ZZPlayAudioBackend b;

  zzplay_prefs_defaults(&prefs);
  prefs.mp3_output = ZZPLAY_AUDIO_MHI;
  prefs.video_audio = ZZPLAY_AUDIO_AX;

  /* 1. Explicit AUDIO= option wins and is strict */
  zzplay_options_init(&options, ZZPLAY_LAUNCH_CLI);
  options.audio_backend = ZZPLAY_AUDIO_AHI;
  options.audio_explicit = 1;

  b = zzplay_prefs_requested_backend(&prefs, &options, ZZPLAY_MEDIA_AUDIO_MP3,
                                     &strict);
  if (b != ZZPLAY_AUDIO_AHI || strict != 1) return 1;

  /* 1b. Explicit AUDIO=AUTO overrides the saved output but may fall back */
  zzplay_options_init(&options, ZZPLAY_LAUNCH_CLI);
  options.audio_backend = ZZPLAY_AUDIO_AUTO;
  options.audio_explicit = 1;
  strict = -1;
  b = zzplay_prefs_requested_backend(&prefs, &options, ZZPLAY_MEDIA_AUDIO_MP2,
                                     &strict);
  if (b != ZZPLAY_AUDIO_AUTO || strict != 0) return 9;

  /* 2. --benchmark (uncapped) wins and is strict */
  zzplay_options_init(&options, ZZPLAY_LAUNCH_CLI);
  options.uncapped = 1;
  options.audio_backend = ZZPLAY_AUDIO_NONE;

  b = zzplay_prefs_requested_backend(&prefs, &options, ZZPLAY_MEDIA_AUDIO_MP3,
                                     &strict);
  if (b != ZZPLAY_AUDIO_NONE || strict != 1) return 2;

  /* 3. Non-explicit: MP3 media uses prefs.mp3_output (non-strict) */
  zzplay_options_init(&options, ZZPLAY_LAUNCH_CLI);
  b = zzplay_prefs_requested_backend(&prefs, &options, ZZPLAY_MEDIA_AUDIO_MP3,
                                     &strict);
  if (b != ZZPLAY_AUDIO_MHI || strict != 0) return 3;

  /* 4. Non-explicit: MP2 media uses prefs.video_audio (non-strict) */
  b = zzplay_prefs_requested_backend(&prefs, &options, ZZPLAY_MEDIA_AUDIO_MP2,
                                     &strict);
  if (b != ZZPLAY_AUDIO_AX || strict != 0) return 4;

  /* 5. Non-explicit: NONE media resolves to AUTO (non-strict) */
  b = zzplay_prefs_requested_backend(&prefs, &options, ZZPLAY_MEDIA_AUDIO_NONE,
                                     &strict);
  if (b != ZZPLAY_AUDIO_AUTO || strict != 0) return 5;

  /* 6. apply_options overrides */
  zzplay_options_init(&options, ZZPLAY_LAUNCH_CLI);
  options.ahi_unit = 2;
  options.mhi_driver = "cli_driver.library";
  options.volume = 40;
  zzplay_prefs_apply_options(&prefs, &options);
  if (prefs.ahi_unit != 2U) return 6;
  if (strcmp(prefs.mhi_driver, "cli_driver.library") != 0) return 7;
  if (prefs.volume != 40U) return 8;

  return 0;
}

static int test_ahiprefs_parser(void)
{
  uint8_t iff[256];
  size_t offset;
  ZZPlayAHIUnitPref units[ZZPLAY_AHIPREFS_UNITS];
  int count;

  /* Build synthetic IFF FORM PREF */
  memset(iff, 0, sizeof(iff));
  memcpy(iff, "FORM", 4);
  /* Size filled later */
  memcpy(iff + 8, "PREF", 4);
  offset = 12;

  /* Chunk 1: AHIU unit 0 */
  memcpy(iff + offset, "AHIU", 4);
  iff[offset + 4] = 0; iff[offset + 5] = 0; iff[offset + 6] = 0;
  iff[offset + 7] = 12; /* size 12 */
  /* data */
  iff[offset + 8] = 0;  /* unit 0 */
  iff[offset + 9] = 0;  /* obsolete */
  iff[offset + 10] = 0; iff[offset + 11] = 2; /* channels = 2 */
  iff[offset + 12] = 0x00; iff[offset + 13] = 0x01;
  iff[offset + 14] = 0x00; iff[offset + 15] = 0x01; /* mode 0x00010001 */
  iff[offset + 16] = 0x00; iff[offset + 17] = 0x00;
  iff[offset + 18] = 0xAC; iff[offset + 19] = 0x44; /* 44100 Hz */
  offset += 20; /* 8 + 12 */

  /* Chunk 2: AHIU unit 255 (music unit, must be ignored) */
  memcpy(iff + offset, "AHIU", 4);
  iff[offset + 4] = 0; iff[offset + 5] = 0; iff[offset + 6] = 0;
  iff[offset + 7] = 12;
  iff[offset + 8] = 255; /* unit 255 */
  iff[offset + 9] = 0;
  iff[offset + 10] = 0; iff[offset + 11] = 4;
  offset += 20;

  /* Chunk 3: AHIU unit 3 */
  memcpy(iff + offset, "AHIU", 4);
  iff[offset + 4] = 0; iff[offset + 5] = 0; iff[offset + 6] = 0;
  iff[offset + 7] = 12;
  iff[offset + 8] = 3;  /* unit 3 */
  iff[offset + 9] = 0;
  iff[offset + 10] = 0; iff[offset + 11] = 2;
  iff[offset + 12] = 0x00; iff[offset + 13] = 0x02;
  iff[offset + 14] = 0x00; iff[offset + 15] = 0x05; /* mode 0x00020005 */
  iff[offset + 16] = 0x00; iff[offset + 17] = 0x00;
  iff[offset + 18] = 0xBB; iff[offset + 19] = 0x80; /* 48000 Hz */
  offset += 20;

  /* Set FORM size (offset - 8) */
  {
    uint32_t form_sz = (uint32_t)(offset - 8U);
    iff[4] = (uint8_t)(form_sz >> 24);
    iff[5] = (uint8_t)(form_sz >> 16);
    iff[6] = (uint8_t)(form_sz >> 8);
    iff[7] = (uint8_t)form_sz;
  }

  count = zzplay_ahiprefs_parse(iff, offset, units);
  if (count != 2) return 1;
  if (!units[0].present || units[0].channels != 2 ||
      units[0].audio_mode != 0x00010001U || units[0].frequency != 44100U)
    return 2;
  if (units[1].present || units[2].present) return 3;
  if (!units[3].present || units[3].channels != 2 ||
      units[3].audio_mode != 0x00020005U || units[3].frequency != 48000U)
    return 4;

  /* Truncated chunk test: declare chunk extending past buffer */
  iff[offset - 20 + 7] = 100; /* Unit 3 size declared 100 bytes */
  count = zzplay_ahiprefs_parse(iff, offset, units);
  /* Scan stops before truncated chunk, so only unit 0 is found */
  if (count != 1) return 5;
  if (!units[0].present || units[3].present) return 6;

  /* Malformed header */
  if (zzplay_ahiprefs_parse((const uint8_t *)"NOT_A_FORM_FILE", 15U, units) != -1)
    return 7;

  return 0;
}

int main(void)
{
  int rc;

  rc = test_prefs_parse_and_format();
  if (rc) {
    fprintf(stderr, "test_prefs_parse_and_format failed: %d\n", rc);
    return 1;
  }
  rc = test_prefs_bad_values();
  if (rc) {
    fprintf(stderr, "test_prefs_bad_values failed: %d\n", rc);
    return 2;
  }
  rc = test_session_read_modify_write();
  if (rc) {
    fprintf(stderr, "test_session_read_modify_write failed: %d\n", rc);
    return 3;
  }
  rc = test_requested_backend_matrix();
  if (rc) {
    fprintf(stderr, "test_requested_backend_matrix failed: %d\n", rc);
    return 4;
  }
  rc = test_ahiprefs_parser();
  if (rc) {
    fprintf(stderr, "test_ahiprefs_parser failed: %d\n", rc);
    return 5;
  }
  rc = test_session_settle_drops_launch_overrides();
  if (rc) {
    fprintf(stderr, "test_session_settle_drops_launch_overrides failed: %d\n",
            rc);
    return 6;
  }

  printf("zzplay_prefs_test: all tests passed\n");
  return 0;
}
