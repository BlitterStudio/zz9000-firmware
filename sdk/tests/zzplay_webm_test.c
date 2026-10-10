/* WebM header probe, envelope and firmware-flag selection.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "../tools/zzplay/zzplay-audio.h"
#include "zz9k/abi.h"
#include "../tools/zzplay/zzplay-prefs.h"
#include "../tools/zzplay/zzplay-probe.h"
#include "../tools/zzplay/zzplay-webm.h"
#include <stdio.h>
#include <string.h>
static int failures;
static void fail(const char *what) { printf("FAIL %s\n", what); failures++; }
static size_t put_id(uint8_t *d, size_t cap, size_t i, uint32_t id)
{
  int len = id > 0xffffffU ? 4 : id > 0xffffU ? 3 : id > 0xffU ? 2 : 1;
  int b;
  if (i + (size_t)len > cap) return (size_t)-1;
  for (b = len - 1; b >= 0; b--) d[i++] = (uint8_t)((id >> (8 * b)) & 0xffU);
  return i;
}
static size_t put_size(uint8_t *d, size_t cap, size_t i, uint32_t sz)
{
  if (sz < 0x7fU) { if (i >= cap) return (size_t)-1; d[i++] = (uint8_t)(0x80U | sz); return i; }
  if (sz < 0x3fffU) {
    if (i + 2U > cap) return (size_t)-1;
    d[i++] = (uint8_t)(0x40U | (sz >> 8)); d[i++] = (uint8_t)sz; return i;
  }
  if (i + 3U > cap) return (size_t)-1;
  d[i++] = (uint8_t)(0x20U | (sz >> 16)); d[i++] = (uint8_t)(sz >> 8); d[i++] = (uint8_t)sz;
  return i;
}
static size_t put_bytes(uint8_t *d, size_t cap, size_t i, uint32_t id, const uint8_t *body, size_t n)
{
  i = put_id(d, cap, i, id);
  if (i == (size_t)-1) return i;
  i = put_size(d, cap, i, (uint32_t)n);
  if (i == (size_t)-1 || i + n > cap) return (size_t)-1;
  if (n && body) memcpy(d + i, body, n);
  return i + n;
}
static size_t put_uint(uint8_t *d, size_t cap, size_t i, uint32_t id, uint32_t value)
{
  uint8_t body[4]; int n = 1, b; uint32_t tmp = value;
  if (value) { n = 0; while (tmp) { n++; tmp >>= 8; } }
  for (b = 0; b < n; b++) body[b] = (uint8_t)(value >> (8 * (n - 1 - b)));
  return put_bytes(d, cap, i, id, body, (size_t)n);
}
static size_t put_str(uint8_t *d, size_t cap, size_t i, uint32_t id, const char *text)
{ return put_bytes(d, cap, i, id, (const uint8_t *)text, strlen(text)); }
static size_t put_master(uint8_t *d, size_t cap, size_t i, uint32_t id, const uint8_t *body, size_t n)
{ return put_bytes(d, cap, i, id, body, n); }
static size_t ebml_header(uint8_t *d, size_t cap, size_t i, const char *doctype)
{
  uint8_t body[64]; size_t n = 0U;
  n = put_uint(body, sizeof(body), n, 0x4286U, 1U);
  n = put_str(body, sizeof(body), n, 0x4282U, doctype);
  return put_master(d, cap, i, 0x1A45DFA3U, body, n);
}
static size_t video_track(uint8_t *d, size_t cap, size_t i, const char *codec,
                          uint32_t w, uint32_t h, uint32_t dw, uint32_t dh,
                          uint32_t default_ns, const uint8_t *roll, size_t roll_n)
{
  uint8_t video[80], track[192], projection[24];
  size_t vn = 0U, tn = 0U, pn;
  vn = put_uint(video, sizeof(video), vn, 0xB0U, w);
  vn = put_uint(video, sizeof(video), vn, 0xBAU, h);
  if (dw && dh) {
    vn = put_uint(video, sizeof(video), vn, 0x54B0U, dw);
    vn = put_uint(video, sizeof(video), vn, 0x54BAU, dh);
  }
  if (roll && roll_n) {
    pn = put_bytes(projection, sizeof(projection), 0U, 0x7675U, roll, roll_n);
    vn = put_master(video, sizeof(video), vn, 0x7670U, projection, pn);
  }
  vn = put_bytes(video, sizeof(video), vn, 0x55B0U, (const uint8_t *)"c", 1U);
  tn = put_uint(track, sizeof(track), tn, 0xD7U, 1U);
  tn = put_uint(track, sizeof(track), tn, 0x83U, 1U);
  tn = put_str(track, sizeof(track), tn, 0x86U, codec);
  if (default_ns) tn = put_uint(track, sizeof(track), tn, 0x23E383U, default_ns);
  tn = put_master(track, sizeof(track), tn, 0xE0U, video, vn);
  return put_master(d, cap, i, 0xAEU, track, tn);
}
static size_t audio_track(uint8_t *d, size_t cap, size_t i, const char *codec,
                          const uint8_t *rate, size_t rate_n, uint32_t channels, uint32_t number)
{
  uint8_t audio[32], track[96]; size_t an = 0U, tn = 0U;
  if (rate && rate_n) an = put_bytes(audio, sizeof(audio), an, 0xB5U, rate, rate_n);
  an = put_uint(audio, sizeof(audio), an, 0x9FU, channels);
  tn = put_uint(track, sizeof(track), tn, 0xD7U, number);
  tn = put_uint(track, sizeof(track), tn, 0x83U, 2U);
  tn = put_str(track, sizeof(track), tn, 0x86U, codec);
  tn = put_master(track, sizeof(track), tn, 0xE1U, audio, an);
  return put_master(d, cap, i, 0xAEU, track, tn);
}
static size_t wrap_segment(uint8_t *d, size_t cap, const uint8_t *body, size_t n, int with_seekhead)
{
  uint8_t inner[1024], info[48], tracks[512];
  size_t m = 0U, in = 0U, trk = 0U, at;
  static const uint8_t duration[] = {0x40, 0xc3, 0x88, 0x00, 0x00, 0x00, 0x00, 0x00};
  if (with_seekhead) {
    m = put_bytes(inner, sizeof(inner), m, 0x114D9B74U, 0, 0U);
    m = put_bytes(inner, sizeof(inner), m, 0xECU, (const uint8_t *)"void", 4U);
  }
  in = put_uint(info, sizeof(info), in, 0x2AD7B1U, 1000000U);
  in = put_bytes(info, sizeof(info), in, 0x4489U, duration, sizeof(duration));
  m = put_master(inner, sizeof(inner), m, 0x1549A966U, info, in);
  trk = put_master(tracks, sizeof(tracks), trk, 0x1654AE6BU, body, n);
  if (m == (size_t)-1 || trk == (size_t)-1 || m + trk > sizeof(inner)) return (size_t)-1;
  memcpy(inner + m, tracks, trk);
  m += trk;
  at = ebml_header(d, cap, 0U, "webm");
  return put_master(d, cap, at, 0x18538067U, inner, m);
}
static int probe_ok(const uint8_t *data, size_t n, ZZPlayWebMInfo *info)
{
  if (!zzplay_probe_webm(data, n, 0, info) || info->refusal != ZZPLAY_WEBM_OK) {
    printf("  refusal %d %lux%lu\n", (int)info->refusal, (unsigned long)info->width, (unsigned long)info->height);
    return 0;
  }
  return 1;
}
static int check_vp8_opus(void)
{
  uint8_t tracks[512], file[1024], line[96];
  size_t tn = 0U, n; ZZPlayWebMInfo info;
  static const uint8_t rate[] = {0x40, 0xe7, 0x70, 0x00, 0x00, 0x00, 0x00, 0x00};
  tn = video_track(tracks, sizeof(tracks), tn, "V_VP8", 640U, 360U, 0, 0, 0, 0, 0);
  tn = audio_track(tracks, sizeof(tracks), tn, "A_OPUS", rate, sizeof(rate), 2U, 2U);
  n = wrap_segment(file, sizeof(file), tracks, tn, 0);
  if (!probe_ok(file, n, &info) || info.video != ZZPLAY_WEBM_VIDEO_VP8 ||
      info.audio != ZZPLAY_WEBM_AUDIO_OPUS || info.width != 640U || info.height != 360U ||
      info.sample_rate != 48000U || info.channels != 2U || info.duration_ms != 10000U) return 0;
  if (!zzplay_webm_format_line(&info, (char *)line, sizeof(line)) ||
      strcmp((char *)line, "WebM VP8 640x360, Opus 48 kHz stereo") != 0) {
    printf("  format '%s'\n", line); return 0;
  }
  return zzplay_webm_realtime(info.video, info.width, info.height);
}
static int check_vp9_vorbis(void)
{
  uint8_t tracks[512], file[1024], line[96];
  size_t tn = 0U, n; ZZPlayWebMInfo info;
  static const uint8_t rate[] = {0x40, 0xe5, 0x88, 0x80, 0x00, 0x00, 0x00, 0x00};
  tn = video_track(tracks, sizeof(tracks), tn, "V_VP9", 320U, 240U, 0, 0, 0, 0, 0);
  tn = audio_track(tracks, sizeof(tracks), tn, "A_VORBIS", rate, sizeof(rate), 1U, 2U);
  n = wrap_segment(file, sizeof(file), tracks, tn, 1);
  if (!probe_ok(file, n, &info) || info.video != ZZPLAY_WEBM_VIDEO_VP9 ||
      info.audio != ZZPLAY_WEBM_AUDIO_VORBIS || info.sample_rate != 44100U || info.channels != 1U) return 0;
  if (!zzplay_webm_format_line(&info, (char *)line, sizeof(line)) ||
      strcmp((char *)line, "WebM VP9 320x240, Vorbis 44.1 kHz mono") != 0) {
    printf("  format '%s'\n", line); return 0;
  }
  return 1;
}
static int check_video_only_portrait(void)
{
  uint8_t tracks[256], file[1024], line[96];
  size_t tn = 0U, n; ZZPlayWebMInfo info; ZZPlayProbeInfo media; FILE *fp;
  tn = video_track(tracks, sizeof(tracks), tn, "V_VP8", 320U, 564U, 0, 0, 33366700U, 0, 0);
  n = wrap_segment(file, sizeof(file), tracks, tn, 1);
  if (!probe_ok(file, n, &info) || info.audio != ZZPLAY_WEBM_AUDIO_NONE ||
      info.width != 320U || info.height != 564U || info.frame_rate_milli != 29970U) {
    printf("  rate %lu\n", (unsigned long)info.frame_rate_milli); return 0;
  }
  if (!zzplay_webm_format_line(&info, (char *)line, sizeof(line)) ||
      strcmp((char *)line, "WebM VP8 320x564, no audio") != 0) return 0;
  fp = tmpfile();
  if (!fp || fwrite(file, 1U, n, fp) != n) { if (fp) fclose(fp); return 0; }
  fflush(fp); rewind(fp);
  if (!zzplay_probe_media_file(fp, &media) || media.kind != ZZPLAY_MEDIA_KIND_WEBM ||
      media.webm.height != 564U) { fclose(fp); return 0; }
  fclose(fp);
  return 1;
}
static int check_portrait_odd(void)
{
  uint8_t tracks[256], file[1024];
  size_t tn = 0U, n; ZZPlayWebMInfo info; uint32_t w, h;
  static const uint8_t roll[] = {0x40, 0x56, 0xa0, 0x00, 0x00, 0x00, 0x00, 0x00};
  tn = video_track(tracks, sizeof(tracks), tn, "V_VP8", 271U, 481U, 9U, 16U, 0, roll, sizeof(roll));
  n = wrap_segment(file, sizeof(file), tracks, tn, 0);
  if (!probe_ok(file, n, &info) || info.width != 271U || info.height != 481U ||
      info.display_width != 9U || info.display_height != 16U ||
      !info.pose_roll_present || info.pose_roll_milli != 90500) {
    printf("  roll %ld display %lux%lu\n", (long)info.pose_roll_milli,
           (unsigned long)info.display_width, (unsigned long)info.display_height);
    return 0;
  }
  zzplay_webm_window_source(&info, &w, &h);
  if (w != 270U || h != 481U) { printf("  window %lux%lu\n", (unsigned long)w, (unsigned long)h); return 0; }
  return zzplay_webm_within_cap(271U, 481U) && zzplay_webm_within_cap(720U, 1280U) &&
         zzplay_webm_within_cap(1080U, 1920U);
}
static int expect_refusal(const uint8_t *data, size_t n, ZZPlayWebMRefusal want, const char *name)
{
  ZZPlayWebMInfo info;
  if (!zzplay_probe_webm(data, n, 0, &info) || info.refusal != want) {
    printf("  %s refusal %d want %d\n", name, (int)info.refusal, (int)want); return 0;
  }
  return 1;
}
static int check_refusals(void)
{
  uint8_t tracks[512], file[1024], matroska[128];
  size_t tn, n, i;
  /* SamplingFrequency as an 8-byte EBML float. */
  static const uint8_t vorbis_4k[] = {0x40, 0xaf, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00};
  static const uint8_t vorbis_8k[] = {0x40, 0xbf, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00};
  static const uint8_t vorbis_44k[] = {0x40, 0xe5, 0x88, 0x80, 0x00, 0x00, 0x00, 0x00};
  static const uint8_t vorbis_96k[] = {0x40, 0xf7, 0x70, 0x00, 0x00, 0x00, 0x00, 0x00};
  static const uint8_t vorbis_192k[] = {0x41, 0x07, 0x70, 0x00, 0x00, 0x00, 0x00, 0x00};
  i = ebml_header(matroska, sizeof(matroska), 0U, "matroska");
  if (!expect_refusal(matroska, i, ZZPLAY_WEBM_MATROSKA, "matroska")) return 0;
  tn = 0; tn = video_track(tracks, sizeof(tracks), tn, "V_VP8", 320, 240, 0, 0, 0, 0, 0);
  tn = video_track(tracks, sizeof(tracks), tn, "V_VP9", 160, 120, 0, 0, 0, 0, 0);
  n = wrap_segment(file, sizeof(file), tracks, tn, 0);
  if (!expect_refusal(file, n, ZZPLAY_WEBM_TWO_VIDEO, "two video")) return 0;
  tn = 0; tn = video_track(tracks, sizeof(tracks), tn, "V_AV1", 320, 240, 0, 0, 0, 0, 0);
  n = wrap_segment(file, sizeof(file), tracks, tn, 0);
  if (!expect_refusal(file, n, ZZPLAY_WEBM_UNKNOWN_CODEC, "av1")) return 0;
  tn = 0; tn = video_track(tracks, sizeof(tracks), tn, "V_VP8", 2560, 1440, 0, 0, 0, 0, 0);
  n = wrap_segment(file, sizeof(file), tracks, tn, 0);
  if (!expect_refusal(file, n, ZZPLAY_WEBM_OVERSIZE, "2560")) return 0;
  tn = 0; tn = video_track(tracks, sizeof(tracks), tn, "V_VP9", 1080, 2400, 0, 0, 0, 0, 0);
  n = wrap_segment(file, sizeof(file), tracks, tn, 0);
  if (!expect_refusal(file, n, ZZPLAY_WEBM_OVERSIZE, "2400")) return 0;
  tn = 0; tn = video_track(tracks, sizeof(tracks), tn, "V_VP8", 640, 360, 0, 0, 0, 0, 0);
  n = wrap_segment(file, sizeof(file), tracks, tn, 0);
  if (n < 40U || !expect_refusal(file, 40U, ZZPLAY_WEBM_TRUNCATED, "trunc")) return 0;
  /* One audio track only: a second would leave the card to pick one. */
  tn = 0; tn = video_track(tracks, sizeof(tracks), tn, "V_VP8", 640, 360, 0, 0, 0, 0, 0);
  tn = audio_track(tracks, sizeof(tracks), tn, "A_OPUS", 0, 0U, 2U, 2U);
  tn = audio_track(tracks, sizeof(tracks), tn, "A_VORBIS", vorbis_44k, sizeof(vorbis_44k), 2U, 3U);
  n = wrap_segment(file, sizeof(file), tracks, tn, 0);
  if (!expect_refusal(file, n, ZZPLAY_WEBM_TWO_AUDIO, "two audio")) return 0;
  /* Audio-only WebM is not a video item. */
  tn = 0; tn = audio_track(tracks, sizeof(tracks), tn, "A_OPUS", 0, 0U, 2U, 1U);
  n = wrap_segment(file, sizeof(file), tracks, tn, 0);
  if (!expect_refusal(file, n, ZZPLAY_WEBM_NO_VIDEO, "no video")) return 0;
  /* The card mixes mono or stereo only. */
  tn = 0; tn = video_track(tracks, sizeof(tracks), tn, "V_VP9", 640, 360, 0, 0, 0, 0, 0);
  tn = audio_track(tracks, sizeof(tracks), tn, "A_OPUS", 0, 0U, 6U, 2U);
  n = wrap_segment(file, sizeof(file), tracks, tn, 0);
  if (!expect_refusal(file, n, ZZPLAY_WEBM_BAD_AUDIO, "6ch")) return 0;
  /* Vorbis is accepted from 8 kHz to 96 kHz inclusive. */
  tn = 0; tn = video_track(tracks, sizeof(tracks), tn, "V_VP8", 640, 360, 0, 0, 0, 0, 0);
  tn = audio_track(tracks, sizeof(tracks), tn, "A_VORBIS", vorbis_4k, sizeof(vorbis_4k), 2U, 2U);
  n = wrap_segment(file, sizeof(file), tracks, tn, 0);
  if (!expect_refusal(file, n, ZZPLAY_WEBM_BAD_AUDIO, "vorbis 4k")) return 0;
  tn = 0; tn = video_track(tracks, sizeof(tracks), tn, "V_VP8", 640, 360, 0, 0, 0, 0, 0);
  tn = audio_track(tracks, sizeof(tracks), tn, "A_VORBIS", vorbis_192k, sizeof(vorbis_192k), 2U, 2U);
  n = wrap_segment(file, sizeof(file), tracks, tn, 0);
  if (!expect_refusal(file, n, ZZPLAY_WEBM_BAD_AUDIO, "vorbis 192k")) return 0;
  tn = 0; tn = video_track(tracks, sizeof(tracks), tn, "V_VP8", 640, 360, 0, 0, 0, 0, 0);
  tn = audio_track(tracks, sizeof(tracks), tn, "A_VORBIS", vorbis_8k, sizeof(vorbis_8k), 2U, 2U);
  n = wrap_segment(file, sizeof(file), tracks, tn, 0);
  if (!expect_refusal(file, n, ZZPLAY_WEBM_OK, "vorbis 8k")) return 0;
  tn = 0; tn = video_track(tracks, sizeof(tracks), tn, "V_VP8", 640, 360, 0, 0, 0, 0, 0);
  tn = audio_track(tracks, sizeof(tracks), tn, "A_VORBIS", vorbis_96k, sizeof(vorbis_96k), 2U, 2U);
  n = wrap_segment(file, sizeof(file), tracks, tn, 0);
  if (!expect_refusal(file, n, ZZPLAY_WEBM_OK, "vorbis 96k")) return 0;
  if (zzplay_webm_within_cap(1921U, 1080U) || zzplay_webm_within_cap(1920U, 1089U) ||
      zzplay_webm_realtime(ZZPLAY_WEBM_VIDEO_VP9, 1280U, 720U)) return 0;
  return 1;
}
static int check_flags_and_backend(void)
{
  uint32_t vp8 = zzplay_webm_required_flags(ZZPLAY_WEBM_VIDEO_VP8, ZZPLAY_WEBM_AUDIO_NONE);
  uint32_t vp9_opus = zzplay_webm_required_flags(ZZPLAY_WEBM_VIDEO_VP9, ZZPLAY_WEBM_AUDIO_OPUS);
  uint32_t vp8_vorbis = zzplay_webm_required_flags(ZZPLAY_WEBM_VIDEO_VP8, ZZPLAY_WEBM_AUDIO_VORBIS);
  ZZPlayAudioAvailability availability; ZZPlayBackendDecision decision;
  ZZPlayPrefs prefs; ZZPlayOptions options; int strict = -1;
  if (zzplay_webm_video_codec_id(ZZPLAY_WEBM_VIDEO_VP8) != ZZ9K_VIDEO_CODEC_VP8 ||
      zzplay_webm_video_codec_id(ZZPLAY_WEBM_VIDEO_VP9) != ZZ9K_VIDEO_CODEC_VP9 ||
      zzplay_webm_audio_codec_id(ZZPLAY_WEBM_AUDIO_OPUS) != ZZ9K_MEDIA_AUDIO_OPUS ||
      zzplay_webm_audio_codec_id(ZZPLAY_WEBM_AUDIO_VORBIS) != ZZ9K_MEDIA_AUDIO_VORBIS) return 0;
  if ((vp8 & ZZ9K_SERVICE_FLAG_VIDEO_WEBM_VP8) == 0U ||
      (vp8 & ZZ9K_SERVICE_FLAG_VIDEO_MEDIA_SESSION) == 0U ||
      (vp9_opus & ZZ9K_SERVICE_FLAG_VIDEO_WEBM_VP9) == 0U ||
      (vp9_opus & ZZ9K_SERVICE_FLAG_VIDEO_MEDIA_OPUS) == 0U ||
      (vp8_vorbis & ZZ9K_SERVICE_FLAG_VIDEO_MEDIA_VORBIS) == 0U ||
      !zzplay_webm_service_ready(vp8, ZZPLAY_WEBM_VIDEO_VP8, ZZPLAY_WEBM_AUDIO_NONE) ||
      zzplay_webm_service_ready(vp8, ZZPLAY_WEBM_VIDEO_VP9, ZZPLAY_WEBM_AUDIO_NONE)) return 0;
  memset(&availability, 0, sizeof(availability));
  availability.ahi = ZZPLAY_BACKEND_FREE; availability.mhi = ZZPLAY_BACKEND_FREE; availability.ax = ZZPLAY_BACKEND_FREE;
  decision = zzplay_audio_select(ZZPLAY_MEDIA_AUDIO_OPUS, ZZPLAY_AUDIO_AUTO, &availability);
  if (decision.status != ZZPLAY_BACKEND_OK || decision.selected != ZZPLAY_AUDIO_AX) return 0;
  availability.ax = ZZPLAY_BACKEND_MISSING;
  decision = zzplay_audio_select(ZZPLAY_MEDIA_AUDIO_VORBIS, ZZPLAY_AUDIO_AUTO, &availability);
  if (decision.selected != ZZPLAY_AUDIO_AHI || !decision.fell_back) return 0;
  decision = zzplay_audio_select(ZZPLAY_MEDIA_AUDIO_OPUS, ZZPLAY_AUDIO_MHI, &availability);
  if (decision.status != ZZPLAY_BACKEND_UNSUPPORTED) return 0;
  zzplay_prefs_defaults(&prefs); prefs.video_audio = ZZPLAY_AUDIO_AHI;
  zzplay_options_init(&options, ZZPLAY_LAUNCH_CLI);
  if (zzplay_prefs_requested_backend(&prefs, &options, ZZPLAY_MEDIA_AUDIO_OPUS, &strict) != ZZPLAY_AUDIO_AHI || strict != 0) return 0;
  return 1;
}
int main(void)
{
  if (!check_vp8_opus()) fail("vp8+opus");
  if (!check_vp9_vorbis()) fail("vp9+vorbis");
  if (!check_video_only_portrait()) fail("portrait");
  if (!check_portrait_odd()) fail("odd");
  if (!check_refusals()) fail("refusals");
  if (!check_flags_and_backend()) fail("flags");
  if (failures) { printf("zzplay_webm_test: %d failure(s)\n", failures); return 1; }
  printf("zzplay_webm_test: all checks passed\n");
  return 0;
}
