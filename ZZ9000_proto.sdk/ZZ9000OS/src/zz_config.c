/*
 * ZZ9000 SD Card Configuration File (ZZ9000.CFG)
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * INI-style parser: one `key = value` per line, `#` or `;` starts a
 * comment, keys and keyword values are case-insensitive. Unknown keys
 * and out-of-range values are logged and skipped so a typo can never
 * take the card down. The parser is pure C (no FatFs/Xilinx calls) and
 * is exercised by the host tests in test/config.
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <ff.h>
#include "xil_cache.h"
#include "zz_config.h"
#include "zz_video_modes.h"

static struct zz_config cfg;

void zz_config_reset(void) {
	memset(&cfg, 0, sizeof(cfg));
}

const struct zz_config* zz_config_get(void) {
	return &cfg;
}

static char lower(char c) {
	return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

static int is_space(char c) {
	return c == ' ' || c == '\t' || c == '\r';
}

/* Case-insensitive match of a NUL-terminated token. */
static int token_eq(const char *s, const char *keyword) {
	while (*keyword) {
		if (lower(*s) != *keyword) return 0;
		s++; keyword++;
	}
	return *s == 0;
}

/* Parse an unsigned decimal number; returns -1 on garbage/overflow. */
static long parse_uint(const char *s) {
	long v = 0;
	if (!*s) return -1;
	while (*s) {
		if (*s < '0' || *s > '9') return -1;
		v = v * 10 + (*s - '0');
		if (v > 0xffff) return -1;
		s++;
	}
	return v;
}

static int parse_onoff(const char *s) {
	if (token_eq(s, "on") || token_eq(s, "1")) return 1;
	if (token_eq(s, "off") || token_eq(s, "0")) return 0;
	return -1;
}

static int hex_nibble(char c) {
	c = lower(c);
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	return -1;
}

/* aa:bb:cc:dd:ee:ff (also accepts '-' separators) */
static int parse_mac(const char *s, uint8_t out[6]) {
	for (int i = 0; i < 6; i++) {
		int hi = hex_nibble(s[0]);
		int lo = hex_nibble(s[1]);
		if (hi < 0 || lo < 0) return -1;
		out[i] = (uint8_t)(hi << 4 | lo);
		s += 2;
		if (i < 5) {
			if (*s != ':' && *s != '-') return -1;
			s++;
		}
	}
	return *s == 0 ? 0 : -1;
}

/* Flat filename in the FAT volume root: no path separators, drive
 * prefixes, or comment introducers ('#' / ';') -- the parser's line
 * splitter drops them mid-value, so a name carrying one could never
 * survive an emit + reparse round trip. Mirrors fw_update's rules. */
static int hdf_name_valid(const char *s) {
	unsigned len = 0;
	if (!*s || *s == '.') return 0;
	for (; *s; s++, len++) {
		char c = *s;
		if (c == '/' || c == '\\' || c == ':' || c == '#' ||
				c == ';' || c < 0x21 || c > 0x7e)
			return 0;
	}
	return len <= ZZ_CONFIG_HDF_NAME_MAX;
}

/* One audio_scene<N>_<field> assignment (plan U5, KTD4). Fields:
 * 0 = lpf, 1..5 = eq01..eq89 band pairs, 6 = out (prefactor+volume),
 * 7 = pan, 8..15 = nm1..nm8 name chunks. The value is range-checked
 * against the scene definition so a corrupt file degrades to the
 * built-in defaults; the bit for the field is set in the scene's
 * mask only after it passes. */
static int audio_scene_key(int scene, int field, const char *value) {
	long v = parse_uint(value);
	long hi, lo;

	if (v < 0 || scene < 0 || scene >= ZZ_CFG_AUDIO_SCENES) return -1;
	switch (field) {
	case 0:
		if (v < 1 || v > 23900) return -1;
		cfg.audio_scene_lpf[scene] = (uint16_t)v;
		break;
	case 1: case 2: case 3: case 4: case 5:
		hi = v / 128; lo = v % 128;
		if (hi > 100 || lo > 100) return -1;
		cfg.audio_scene_eq[scene][field - 1] = (uint16_t)v;
		break;
	case 6:
		hi = v / 128; lo = v % 128;
		if (hi > 100 || lo > 100) return -1;
		cfg.audio_scene_out[scene] = (uint16_t)v;
		break;
	case 7:
		if (v > 100) return -1;
		cfg.audio_scene_pan[scene] = (uint16_t)v;
		break;
	case 8: case 9: case 10: case 11:
	case 12: case 13: case 14: case 15:
		/* Name chunk, same packing as the SCENE_WRITE NAME param:
		 * c1*256+c2, both printable ASCII or 0 (a 0x0000 chunk is
		 * the terminator); a NUL first char only exists in the
		 * pure terminator. */
		if (v > 0xffff) return -1;
		hi = v / 256; lo = v % 256;
		if (hi != 0 && (hi < 0x20 || hi > 0x7e)) return -1;
		if (lo != 0 && (lo < 0x20 || lo > 0x7e)) return -1;
		if (hi == 0 && lo != 0) return -1;
		cfg.audio_scene_nm[scene][field - 8] = (uint16_t)v;
		break;
	default:
		return -1;
	}
	cfg.audio_scene_mask[scene] |= (uint16_t)(1u << field);
	return 0;
}

static int apply_key(const char *key, const char *value) {
	/* Preferred user-facing native-video setting.  It replaces three
	 * independently editable legacy keys with one coherent profile, while
	 * still filling the same internal fields so older driver/runtime paths
	 * keep their established behaviour.  Legacy keys remain accepted below;
	 * normal last-value-wins parsing therefore also works for mixed files. */
	if (token_eq(key, "videocap_profile")) {
		int output_profile = ZZ_VIDEOCAP_OUTPUT_FULL_60;

		if (token_eq(value, "full_60")) {
			cfg.videocap_mode = ZZVMODE_800x600;
			cfg.videocap_shres = 1;
			cfg.ns_vsync = 0;
		} else if (token_eq(value, "full_exact")) {
			cfg.videocap_mode = ZZVMODE_800x600;
			cfg.videocap_shres = 1;
			cfg.ns_vsync = 1;
		} else if (token_eq(value, "filtered_60")) {
			cfg.videocap_mode = ZZVMODE_800x600;
			cfg.videocap_shres = 0;
			cfg.ns_vsync = 0;
		} else if (token_eq(value, "filtered_pal")) {
			cfg.videocap_mode = ZZVMODE_720x576;
			cfg.videocap_shres = 0;
			cfg.ns_vsync = 0;
		} else if (token_eq(value, "filtered_pal_exact")) {
			cfg.videocap_mode = ZZVMODE_720x576;
			cfg.videocap_shres = 0;
			cfg.ns_vsync = 1;
		} else if (token_eq(value, "filtered_ntsc_exact")) {
			cfg.videocap_mode = ZZVMODE_720x576;
			cfg.videocap_shres = 0;
			cfg.ns_vsync = 2;
		} else if (token_eq(value, "centered_1080p_60")) {
			cfg.videocap_mode = ZZVMODE_800x600;
			cfg.videocap_shres = 1;
			cfg.ns_vsync = 0;
			output_profile = ZZ_VIDEOCAP_OUTPUT_CENTERED_1080P_60;
		} else {
			return -1;
		}
		cfg.videocap_mode_present = 1;
		cfg.videocap_shres_present = 1;
		cfg.ns_vsync_present = 1;
		cfg.videocap_output_profile = (uint8_t)output_profile;
		return 0;
	}
	if (token_eq(key, "videocap_mode")) {
		if (token_eq(value, "800x600")) {
			cfg.videocap_mode = ZZVMODE_800x600;
		} else if (token_eq(value, "pal") || token_eq(value, "720x576")) {
			cfg.videocap_mode = ZZVMODE_720x576;
		} else {
			return -1;
		}
		cfg.videocap_mode_present = 1;
		/* Before videocap_shres existed, both legacy output modes described
		 * the filtered capture path. Preserve that meaning while retaining
		 * normal last-value-wins behavior for mixed old/new files. */
		cfg.videocap_shres = 0;
		cfg.videocap_shres_present = 1;
		cfg.videocap_output_profile = ZZ_VIDEOCAP_OUTPUT_FULL_60;
		return 0;
	}
	if (token_eq(key, "videocap_sample")) {
		if (token_eq(value, "average")) cfg.videocap_sample = 0;
		else if (token_eq(value, "even")) cfg.videocap_sample = 1;
		else if (token_eq(value, "odd")) cfg.videocap_sample = 2;
		else return -1;
		cfg.videocap_sample_present = 1;
		return 0;
	}
	if (token_eq(key, "videocap_shres")) {
		if (token_eq(value, "filter")) cfg.videocap_shres = 0;
		else if (token_eq(value, "full")) cfg.videocap_shres = 1;
		else return -1;
		cfg.videocap_shres_present = 1;
		cfg.videocap_output_profile = ZZ_VIDEOCAP_OUTPUT_FULL_60;
		return 0;
	}
	if (token_eq(key, "videocap_crop_h")) {
		long v = parse_uint(value);
		if (v < 0 || v > 4095) return -1;
		cfg.videocap_crop_h = (uint16_t)v;
		cfg.videocap_crop_h_present = 1;
		return 0;
	}
	if (token_eq(key, "videocap_crop_v")) {
		long v = parse_uint(value);
		if (v < 0 || v > 4095) return -1;
		cfg.videocap_crop_v = (uint16_t)v;
		cfg.videocap_crop_v_present = 1;
		return 0;
	}
	if (token_eq(key, "nonstandard_vsync")) {
		if (token_eq(value, "off")) cfg.ns_vsync = 0;
		else if (token_eq(value, "pal") || token_eq(value, "on")) cfg.ns_vsync = 1;
		else if (token_eq(value, "ntsc")) cfg.ns_vsync = 2;
		else return -1;
		cfg.ns_vsync_present = 1;
		cfg.videocap_output_profile = ZZ_VIDEOCAP_OUTPUT_FULL_60;
		return 0;
	}
	if (token_eq(key, "scanline_mode")) {
		long v = parse_uint(value);
		if (v < 0 || v > 3) return -1;
		cfg.scanline_mode = (uint16_t)v;
		cfg.scanline_mode_present = 1;
		return 0;
	}
	if (token_eq(key, "scanline_parity")) {
		long v = parse_uint(value);
		if (v < 0 || v > 1) return -1;
		cfg.scanline_parity = (uint16_t)v;
		cfg.scanline_parity_present = 1;
		return 0;
	}
	if (token_eq(key, "int2")) {
		int v = parse_onoff(value);
		if (v < 0) return -1;
		cfg.int2 = (uint16_t)v;
		cfg.int2_present = 1;
		return 0;
	}
	if (token_eq(key, "mac")) {
		uint8_t mac[6];
		if (parse_mac(value, mac) != 0) return -1;
		memcpy(cfg.mac, mac, 6);
		cfg.mac_present = 1;
		return 0;
	}
	if (token_eq(key, "offscreen_bitmaps")) {
		int v = parse_onoff(value);
		if (v < 0) return -1;
		cfg.offscreen_bitmaps = (uint16_t)v;
		cfg.offscreen_bitmaps_present = 1;
		return 0;
	}
	if (token_eq(key, "video_overlay")) {
		int v = parse_onoff(value);
		if (v < 0) return -1;
		cfg.video_overlay = (uint16_t)v;
		cfg.video_overlay_present = 1;
		return 0;
	}

	/* ---- audio control-plane keys (plan U5, KTD4) ----
	 *
	 * Decimal-only grammar, each value <= 0xffff, so every scene
	 * serializes as a small group of keys: audio_scene<N>_lpf,
	 * _eq01/_eq23/_eq45/_eq67/_eq89 (band pairs packed hi*128+lo),
	 * _out (prefactor*128+volume), _pan and _nm1.._nm8 (the label,
	 * two ASCII chars per key packed c1*256+c2). Values are
	 * range-checked here so a corrupt file degrades to the scene
	 * module's built-in defaults. */
	if (token_eq(key, "audio_active")) {
		long v = parse_uint(value);
		if (v < 0 || v >= ZZ_CFG_AUDIO_SCENES) return -1;
		cfg.audio_active = (uint16_t)v;
		cfg.audio_active_present = 1;
		return 0;
	}
	if (token_eq(key, "audio_baseline")) {
		long v = parse_uint(value);
		if (v < 0) return -1;
		cfg.audio_baseline = (uint16_t)v;
		cfg.audio_baseline_present = 1;
		return 0;
	}
	if (token_eq(key, "audio_ceiling_paula")) {
		long v = parse_uint(value);
		if (v < ZZ_CFG_AUDIO_CEILING_MIN ||
				v > ZZ_CFG_AUDIO_CEILING_MAX)
			return -1;
		cfg.audio_ceiling_paula = (uint16_t)v;
		cfg.audio_ceiling_paula_present = 1;
		return 0;
	}
	if (token_eq(key, "audio_ceiling_ax")) {
		long v = parse_uint(value);
		if (v < ZZ_CFG_AUDIO_CEILING_MIN ||
				v > ZZ_CFG_AUDIO_CEILING_MAX)
			return -1;
		cfg.audio_ceiling_ax = (uint16_t)v;
		cfg.audio_ceiling_ax_present = 1;
		return 0;
	}
	if (token_eq(key, "audio_scene0_lpf"))  return audio_scene_key(0, 0, value);
	if (token_eq(key, "audio_scene0_eq01")) return audio_scene_key(0, 1, value);
	if (token_eq(key, "audio_scene0_eq23")) return audio_scene_key(0, 2, value);
	if (token_eq(key, "audio_scene0_eq45")) return audio_scene_key(0, 3, value);
	if (token_eq(key, "audio_scene0_eq67")) return audio_scene_key(0, 4, value);
	if (token_eq(key, "audio_scene0_eq89")) return audio_scene_key(0, 5, value);
	if (token_eq(key, "audio_scene0_out"))  return audio_scene_key(0, 6, value);
	if (token_eq(key, "audio_scene0_pan"))  return audio_scene_key(0, 7, value);
	if (token_eq(key, "audio_scene0_nm1"))  return audio_scene_key(0, 8, value);
	if (token_eq(key, "audio_scene0_nm2"))  return audio_scene_key(0, 9, value);
	if (token_eq(key, "audio_scene0_nm3"))  return audio_scene_key(0, 10, value);
	if (token_eq(key, "audio_scene0_nm4"))  return audio_scene_key(0, 11, value);
	if (token_eq(key, "audio_scene0_nm5"))  return audio_scene_key(0, 12, value);
	if (token_eq(key, "audio_scene0_nm6"))  return audio_scene_key(0, 13, value);
	if (token_eq(key, "audio_scene0_nm7"))  return audio_scene_key(0, 14, value);
	if (token_eq(key, "audio_scene0_nm8"))  return audio_scene_key(0, 15, value);
	if (token_eq(key, "audio_scene1_lpf"))  return audio_scene_key(1, 0, value);
	if (token_eq(key, "audio_scene1_eq01")) return audio_scene_key(1, 1, value);
	if (token_eq(key, "audio_scene1_eq23")) return audio_scene_key(1, 2, value);
	if (token_eq(key, "audio_scene1_eq45")) return audio_scene_key(1, 3, value);
	if (token_eq(key, "audio_scene1_eq67")) return audio_scene_key(1, 4, value);
	if (token_eq(key, "audio_scene1_eq89")) return audio_scene_key(1, 5, value);
	if (token_eq(key, "audio_scene1_out"))  return audio_scene_key(1, 6, value);
	if (token_eq(key, "audio_scene1_pan"))  return audio_scene_key(1, 7, value);
	if (token_eq(key, "audio_scene1_nm1"))  return audio_scene_key(1, 8, value);
	if (token_eq(key, "audio_scene1_nm2"))  return audio_scene_key(1, 9, value);
	if (token_eq(key, "audio_scene1_nm3"))  return audio_scene_key(1, 10, value);
	if (token_eq(key, "audio_scene1_nm4"))  return audio_scene_key(1, 11, value);
	if (token_eq(key, "audio_scene1_nm5"))  return audio_scene_key(1, 12, value);
	if (token_eq(key, "audio_scene1_nm6"))  return audio_scene_key(1, 13, value);
	if (token_eq(key, "audio_scene1_nm7"))  return audio_scene_key(1, 14, value);
	if (token_eq(key, "audio_scene1_nm8"))  return audio_scene_key(1, 15, value);
	if (token_eq(key, "audio_scene2_lpf"))  return audio_scene_key(2, 0, value);
	if (token_eq(key, "audio_scene2_eq01")) return audio_scene_key(2, 1, value);
	if (token_eq(key, "audio_scene2_eq23")) return audio_scene_key(2, 2, value);
	if (token_eq(key, "audio_scene2_eq45")) return audio_scene_key(2, 3, value);
	if (token_eq(key, "audio_scene2_eq67")) return audio_scene_key(2, 4, value);
	if (token_eq(key, "audio_scene2_eq89")) return audio_scene_key(2, 5, value);
	if (token_eq(key, "audio_scene2_out"))  return audio_scene_key(2, 6, value);
	if (token_eq(key, "audio_scene2_pan"))  return audio_scene_key(2, 7, value);
	if (token_eq(key, "audio_scene2_nm1"))  return audio_scene_key(2, 8, value);
	if (token_eq(key, "audio_scene2_nm2"))  return audio_scene_key(2, 9, value);
	if (token_eq(key, "audio_scene2_nm3"))  return audio_scene_key(2, 10, value);
	if (token_eq(key, "audio_scene2_nm4"))  return audio_scene_key(2, 11, value);
	if (token_eq(key, "audio_scene2_nm5"))  return audio_scene_key(2, 12, value);
	if (token_eq(key, "audio_scene2_nm6"))  return audio_scene_key(2, 13, value);
	if (token_eq(key, "audio_scene2_nm7"))  return audio_scene_key(2, 14, value);
	if (token_eq(key, "audio_scene2_nm8"))  return audio_scene_key(2, 15, value);
	if (token_eq(key, "audio_scene3_lpf"))  return audio_scene_key(3, 0, value);
	if (token_eq(key, "audio_scene3_eq01")) return audio_scene_key(3, 1, value);
	if (token_eq(key, "audio_scene3_eq23")) return audio_scene_key(3, 2, value);
	if (token_eq(key, "audio_scene3_eq45")) return audio_scene_key(3, 3, value);
	if (token_eq(key, "audio_scene3_eq67")) return audio_scene_key(3, 4, value);
	if (token_eq(key, "audio_scene3_eq89")) return audio_scene_key(3, 5, value);
	if (token_eq(key, "audio_scene3_out"))  return audio_scene_key(3, 6, value);
	if (token_eq(key, "audio_scene3_pan"))  return audio_scene_key(3, 7, value);
	if (token_eq(key, "audio_scene3_nm1"))  return audio_scene_key(3, 8, value);
	if (token_eq(key, "audio_scene3_nm2"))  return audio_scene_key(3, 9, value);
	if (token_eq(key, "audio_scene3_nm3"))  return audio_scene_key(3, 10, value);
	if (token_eq(key, "audio_scene3_nm4"))  return audio_scene_key(3, 11, value);
	if (token_eq(key, "audio_scene3_nm5"))  return audio_scene_key(3, 12, value);
	if (token_eq(key, "audio_scene3_nm6"))  return audio_scene_key(3, 13, value);
	if (token_eq(key, "audio_scene3_nm7"))  return audio_scene_key(3, 14, value);
	if (token_eq(key, "audio_scene3_nm8"))  return audio_scene_key(3, 15, value);
	if (token_eq(key, "audio_scene4_lpf"))  return audio_scene_key(4, 0, value);
	if (token_eq(key, "audio_scene4_eq01")) return audio_scene_key(4, 1, value);
	if (token_eq(key, "audio_scene4_eq23")) return audio_scene_key(4, 2, value);
	if (token_eq(key, "audio_scene4_eq45")) return audio_scene_key(4, 3, value);
	if (token_eq(key, "audio_scene4_eq67")) return audio_scene_key(4, 4, value);
	if (token_eq(key, "audio_scene4_eq89")) return audio_scene_key(4, 5, value);
	if (token_eq(key, "audio_scene4_out"))  return audio_scene_key(4, 6, value);
	if (token_eq(key, "audio_scene4_pan"))  return audio_scene_key(4, 7, value);
	if (token_eq(key, "audio_scene4_nm1"))  return audio_scene_key(4, 8, value);
	if (token_eq(key, "audio_scene4_nm2"))  return audio_scene_key(4, 9, value);
	if (token_eq(key, "audio_scene4_nm3"))  return audio_scene_key(4, 10, value);
	if (token_eq(key, "audio_scene4_nm4"))  return audio_scene_key(4, 11, value);
	if (token_eq(key, "audio_scene4_nm5"))  return audio_scene_key(4, 12, value);
	if (token_eq(key, "audio_scene4_nm6"))  return audio_scene_key(4, 13, value);
	if (token_eq(key, "audio_scene4_nm7"))  return audio_scene_key(4, 14, value);
	if (token_eq(key, "audio_scene4_nm8"))  return audio_scene_key(4, 15, value);
	if (token_eq(key, "audio_scene5_lpf"))  return audio_scene_key(5, 0, value);
	if (token_eq(key, "audio_scene5_eq01")) return audio_scene_key(5, 1, value);
	if (token_eq(key, "audio_scene5_eq23")) return audio_scene_key(5, 2, value);
	if (token_eq(key, "audio_scene5_eq45")) return audio_scene_key(5, 3, value);
	if (token_eq(key, "audio_scene5_eq67")) return audio_scene_key(5, 4, value);
	if (token_eq(key, "audio_scene5_eq89")) return audio_scene_key(5, 5, value);
	if (token_eq(key, "audio_scene5_out"))  return audio_scene_key(5, 6, value);
	if (token_eq(key, "audio_scene5_pan"))  return audio_scene_key(5, 7, value);
	if (token_eq(key, "audio_scene5_nm1"))  return audio_scene_key(5, 8, value);
	if (token_eq(key, "audio_scene5_nm2"))  return audio_scene_key(5, 9, value);
	if (token_eq(key, "audio_scene5_nm3"))  return audio_scene_key(5, 10, value);
	if (token_eq(key, "audio_scene5_nm4"))  return audio_scene_key(5, 11, value);
	if (token_eq(key, "audio_scene5_nm5"))  return audio_scene_key(5, 12, value);
	if (token_eq(key, "audio_scene5_nm6"))  return audio_scene_key(5, 13, value);
	if (token_eq(key, "audio_scene5_nm7"))  return audio_scene_key(5, 14, value);
	if (token_eq(key, "audio_scene5_nm8"))  return audio_scene_key(5, 15, value);
	if (token_eq(key, "audio_scene6_lpf"))  return audio_scene_key(6, 0, value);
	if (token_eq(key, "audio_scene6_eq01")) return audio_scene_key(6, 1, value);
	if (token_eq(key, "audio_scene6_eq23")) return audio_scene_key(6, 2, value);
	if (token_eq(key, "audio_scene6_eq45")) return audio_scene_key(6, 3, value);
	if (token_eq(key, "audio_scene6_eq67")) return audio_scene_key(6, 4, value);
	if (token_eq(key, "audio_scene6_eq89")) return audio_scene_key(6, 5, value);
	if (token_eq(key, "audio_scene6_out"))  return audio_scene_key(6, 6, value);
	if (token_eq(key, "audio_scene6_pan"))  return audio_scene_key(6, 7, value);
	if (token_eq(key, "audio_scene6_nm1"))  return audio_scene_key(6, 8, value);
	if (token_eq(key, "audio_scene6_nm2"))  return audio_scene_key(6, 9, value);
	if (token_eq(key, "audio_scene6_nm3"))  return audio_scene_key(6, 10, value);
	if (token_eq(key, "audio_scene6_nm4"))  return audio_scene_key(6, 11, value);
	if (token_eq(key, "audio_scene6_nm5"))  return audio_scene_key(6, 12, value);
	if (token_eq(key, "audio_scene6_nm6"))  return audio_scene_key(6, 13, value);
	if (token_eq(key, "audio_scene6_nm7"))  return audio_scene_key(6, 14, value);
	if (token_eq(key, "audio_scene6_nm8"))  return audio_scene_key(6, 15, value);
	if (token_eq(key, "audio_scene7_lpf"))  return audio_scene_key(7, 0, value);
	if (token_eq(key, "audio_scene7_eq01")) return audio_scene_key(7, 1, value);
	if (token_eq(key, "audio_scene7_eq23")) return audio_scene_key(7, 2, value);
	if (token_eq(key, "audio_scene7_eq45")) return audio_scene_key(7, 3, value);
	if (token_eq(key, "audio_scene7_eq67")) return audio_scene_key(7, 4, value);
	if (token_eq(key, "audio_scene7_eq89")) return audio_scene_key(7, 5, value);
	if (token_eq(key, "audio_scene7_out"))  return audio_scene_key(7, 6, value);
	if (token_eq(key, "audio_scene7_pan"))  return audio_scene_key(7, 7, value);
	if (token_eq(key, "audio_scene7_nm1"))  return audio_scene_key(7, 8, value);
	if (token_eq(key, "audio_scene7_nm2"))  return audio_scene_key(7, 9, value);
	if (token_eq(key, "audio_scene7_nm3"))  return audio_scene_key(7, 10, value);
	if (token_eq(key, "audio_scene7_nm4"))  return audio_scene_key(7, 11, value);
	if (token_eq(key, "audio_scene7_nm5"))  return audio_scene_key(7, 12, value);
	if (token_eq(key, "audio_scene7_nm6"))  return audio_scene_key(7, 13, value);
	if (token_eq(key, "audio_scene7_nm7"))  return audio_scene_key(7, 14, value);
	if (token_eq(key, "audio_scene7_nm8"))  return audio_scene_key(7, 15, value);
	if (token_eq(key, "hdf")) {
		if (!hdf_name_valid(value)) return -1;
		cfg.hdf_path[0] = '0';
		cfg.hdf_path[1] = ':';
		cfg.hdf_path[2] = '/';
		strcpy(&cfg.hdf_path[3], value);
		cfg.hdf_present = 1;
		return 0;
	}
	return -2; /* unknown key */
}

int zz_config_parse(const char *text, unsigned len) {
	int accepted = 0;
	unsigned pos = 0;
	int lineno = 0;

	while (pos < len) {
		char line[128];
		unsigned n = 0;
		lineno++;

		/* copy one line, dropping comments and the terminator */
		int in_comment = 0;
		while (pos < len && text[pos] != '\n') {
			char c = text[pos++];
			if (c == '#' || c == ';') in_comment = 1;
			if (!in_comment && n < sizeof(line) - 1) line[n++] = c;
		}
		if (pos < len) pos++; /* skip '\n' */
		line[n] = 0;

		/* trim trailing whitespace */
		while (n > 0 && is_space(line[n - 1])) line[--n] = 0;

		char *p = line;
		while (is_space(*p)) p++;
		if (!*p) continue;

		char *eq = strchr(p, '=');
		if (!eq) {
			printf("[CFG] line %d: not `key = value`, skipped\n", lineno);
			continue;
		}

		/* split and trim key */
		char *key_end = eq;
		while (key_end > p && is_space(key_end[-1])) key_end--;
		*key_end = 0;

		char *value = eq + 1;
		while (is_space(*value)) value++;

		if (!*p || !*value) {
			printf("[CFG] line %d: empty key or value, skipped\n", lineno);
			continue;
		}

		int r = apply_key(p, value);
		if (r == 0) {
			accepted++;
		} else if (r == -2) {
			printf("[CFG] line %d: unknown key '%s', skipped\n", lineno, p);
		} else {
			printf("[CFG] line %d: bad value '%s' for '%s', skipped\n",
			       lineno, value, p);
		}
	}
	return accepted;
}

/* Persistence paths on the SD volume: the loader reads the file (and,
 * when it is missing, the backup the save path keeps); the writer
 * stages ZZCFG.TMP and renames it over ZZ9000.CFG. */
#define ZZ_CONFIG_FILE_PATH   "0:/" ZZ_CONFIG_FILENAME
#define ZZ_CONFIG_TEMP_PATH   "0:/ZZCFG.TMP"
#define ZZ_CONFIG_BAK_FILENAME "ZZ9000.BAK"
#define ZZ_CONFIG_BAK_PATH    "0:/" ZZ_CONFIG_BAK_FILENAME

int zz_config_load(void) {
	static FATFS cfg_fs;
	static char buf[ZZ_CONFIG_MAX_SIZE];
	FIL f;
	UINT nread = 0;
	FRESULT fr;

	zz_config_reset();

	fr = f_mount(&cfg_fs, "0:/", 1);
	if (fr != FR_OK) {
		printf("[CFG] f_mount failed: %d (no card / not FAT?), using defaults\n",
		       (int)fr);
		return -1;
	}

	fr = f_open(&f, ZZ_CONFIG_FILE_PATH, FA_READ);
	if (fr == FR_NO_FILE || fr == FR_NO_PATH) {
		/* The save path keeps the previous file as ZZ9000.BAK, so a
		 * missing CFG with a BAK present means the last save died
		 * between the backup and commit renames -- recover from the
		 * backup instead of silently booting defaults. */
		printf("[CFG] *** no " ZZ_CONFIG_FILENAME " (%d); recovering "
			"from " ZZ_CONFIG_BAK_FILENAME " ***\n", (int)fr);
		fr = f_open(&f, ZZ_CONFIG_BAK_PATH, FA_READ);
	}
	if (fr != FR_OK) {
		printf("[CFG] no " ZZ_CONFIG_FILENAME " (%d), using defaults\n", (int)fr);
		f_mount(0, "0:/", 0);
		return -1;
	}

	fr = f_read(&f, buf, sizeof(buf) - 1, &nread);
	f_close(&f);
	f_mount(0, "0:/", 0);

	if (fr != FR_OK) {
		printf("[CFG] read of " ZZ_CONFIG_FILENAME " failed: %d\n", (int)fr);
		return -1;
	}
	if (nread == sizeof(buf) - 1) {
		printf("[CFG] warning: " ZZ_CONFIG_FILENAME " larger than %u bytes, tail ignored\n",
		       (unsigned)(sizeof(buf) - 1));
		/* The audio keys serialize last, so an oversized file drops
		 * them first. Make the truncation queryable (U5). */
		cfg.truncated = 1;
	}
	buf[nread] = 0;

	int n = zz_config_parse(buf, nread);
	cfg.loaded = 1;
	printf("[CFG] " ZZ_CONFIG_FILENAME ": %d option(s) set\n", n);
	return 0;
}

uint16_t zz_config_read_raw(void *buffer, uint32_t max_len, uint32_t *out_len) {
	FIL f;
	UINT nread = 0;
	FRESULT fr;

	*out_len = 0;

	/* The FAT volume is registered by sd_storage_init() at this point.
	 * During the save machine's one-loop rename gap, the previous live
	 * file is already ZZ9000.BAK; read it as the coherent old snapshot
	 * rather than reporting a transient NO_FILE. */
	fr = f_open(&f, ZZ_CONFIG_FILE_PATH, FA_READ);
	if (fr == FR_NO_FILE || fr == FR_NO_PATH)
		fr = f_open(&f, ZZ_CONFIG_BAK_PATH, FA_READ);
	if (fr == FR_NO_FILE || fr == FR_NO_PATH)
		return ZZ_CONFIG_FILE_NO_FILE;
	if (fr != FR_OK) {
		printf("[CFG] raw read open failed: %d\n", (int)fr);
		return ZZ_CONFIG_FILE_IO_ERROR;
	}

	fr = f_read(&f, buffer, max_len, &nread);
	f_close(&f);
	if (fr != FR_OK) {
		printf("[CFG] raw read failed: %d\n", (int)fr);
		return ZZ_CONFIG_FILE_IO_ERROR;
	}

	*out_len = nread;
	return ZZ_CONFIG_FILE_OK;
}

uint16_t zz_config_query(uint16_t key, uint16_t *present) {
	uint16_t p = 0, v = 0;

	switch (key) {
	case ZZ_CONFIG_KEY_LOADED:
		p = cfg.loaded;
		v = cfg.loaded;
		break;
	case ZZ_CONFIG_KEY_VIDEOCAP_MODE:
		p = cfg.videocap_mode_present;
		v = cfg.videocap_mode;
		if (cfg.videocap_output_profile ==
		    ZZ_VIDEOCAP_OUTPUT_CENTERED_1080P_60)
			v = ZZVMODE_1920x1080_60;
		break;
	case ZZ_CONFIG_KEY_VIDEOCAP_SAMPLE:
		p = cfg.videocap_sample_present;
		v = cfg.videocap_sample;
		break;
	case ZZ_CONFIG_KEY_VIDEOCAP_SHRES:
		p = cfg.videocap_shres_present;
		v = cfg.videocap_shres;
		break;
	case ZZ_CONFIG_KEY_VIDEOCAP_CROP_H:
		p = cfg.videocap_crop_h_present;
		v = cfg.videocap_crop_h;
		break;
	case ZZ_CONFIG_KEY_VIDEOCAP_CROP_V:
		p = cfg.videocap_crop_v_present;
		v = cfg.videocap_crop_v;
		break;
	case ZZ_CONFIG_KEY_NS_VSYNC:
		p = cfg.ns_vsync_present;
		v = cfg.ns_vsync;
		break;
	case ZZ_CONFIG_KEY_SCANLINE_MODE:
		p = cfg.scanline_mode_present;
		v = cfg.scanline_mode;
		break;
	case ZZ_CONFIG_KEY_SCANLINE_PARITY:
		p = cfg.scanline_parity_present;
		v = cfg.scanline_parity;
		break;
	case ZZ_CONFIG_KEY_INT2:
		p = cfg.int2_present;
		v = cfg.int2;
		break;
	case ZZ_CONFIG_KEY_MAC_HI:
		p = cfg.mac_present;
		v = (uint16_t)(cfg.mac[0] << 8 | cfg.mac[1]);
		break;
	case ZZ_CONFIG_KEY_MAC_MID:
		p = cfg.mac_present;
		v = (uint16_t)(cfg.mac[2] << 8 | cfg.mac[3]);
		break;
	case ZZ_CONFIG_KEY_MAC_LO:
		p = cfg.mac_present;
		v = (uint16_t)(cfg.mac[4] << 8 | cfg.mac[5]);
		break;
	case ZZ_CONFIG_KEY_OFFSCREEN_BITMAPS:
		p = cfg.offscreen_bitmaps_present;
		v = cfg.offscreen_bitmaps;
		break;
	case ZZ_CONFIG_KEY_VIDEO_OVERLAY:
		p = cfg.video_overlay_present;
		v = cfg.video_overlay;
		break;
	case ZZ_CONFIG_KEY_AUDIO_TRUNCATED:
		p = cfg.loaded;
		v = cfg.truncated;
		break;
	default:
		break;
	}

	if (present) *present = p;
	return p ? v : 0;
}
/* ---- persistence writer (plan U5, KTD5) ---- */

static int cfg_save_temp_pending; /* ZZCFG.TMP exists from this boot */

/* The canonical profile name for the parsed legacy tuple, mirroring
 * the driver editor's zzcfg_profile_from_legacy recovery table. */
static const char *videocap_profile_name(void) {
	int pal = cfg.videocap_mode == ZZVMODE_720x576;
	int full = cfg.videocap_shres != 0;
	int vsync = cfg.ns_vsync;

	if (cfg.videocap_output_profile ==
	    ZZ_VIDEOCAP_OUTPUT_CENTERED_1080P_60)
		return "centered_1080p_60";
	if (full) return vsync ? "full_exact" : "full_60";
	if (pal && vsync == 1) return "filtered_pal_exact";
	if (pal && vsync == 2) return "filtered_ntsc_exact";
	if (pal) return "filtered_pal";
	return "filtered_60";
}

static int emit_line(char *buf, unsigned size, int off, const char *fmt,
		...) {
	va_list ap;
	int n;

	if (off < 0 || (unsigned)off >= size) return -1;
	va_start(ap, fmt);
	n = vsnprintf(buf + off, size - off, fmt, ap);
	va_end(ap);
	if (n < 0 || (unsigned)n >= size - off) return -1;
	return off + n;
}

int zz_config_emit_present_keys(char *buf, unsigned size, int off) {
	static const char *sample_names[] = { "average", "even", "odd" };

	if (size == 0 || off < 0 || (unsigned)off >= size) return -1;
	buf[off] = 0;

#define EMIT(...) do { \
		off = emit_line(buf, size, off, __VA_ARGS__); \
		if (off < 0) return -1; \
	} while (0)

	if (cfg.videocap_mode_present || cfg.videocap_shres_present ||
	    cfg.ns_vsync_present)
		EMIT("videocap_profile = %s\n", videocap_profile_name());
	if (cfg.videocap_sample_present)
		EMIT("videocap_sample = %s\n",
			sample_names[cfg.videocap_sample <= 2
				? cfg.videocap_sample : 0]);
	if (cfg.videocap_crop_h_present)
		EMIT("videocap_crop_h = %u\n", (unsigned)cfg.videocap_crop_h);
	if (cfg.videocap_crop_v_present)
		EMIT("videocap_crop_v = %u\n", (unsigned)cfg.videocap_crop_v);
	if (cfg.scanline_mode_present)
		EMIT("scanline_mode = %u\n", (unsigned)cfg.scanline_mode);
	if (cfg.scanline_parity_present)
		EMIT("scanline_parity = %u\n", (unsigned)cfg.scanline_parity);
	if (cfg.int2_present)
		EMIT("int2 = %s\n", cfg.int2 ? "on" : "off");
	if (cfg.offscreen_bitmaps_present)
		EMIT("offscreen_bitmaps = %s\n",
			cfg.offscreen_bitmaps ? "on" : "off");
	if (cfg.video_overlay_present)
		EMIT("video_overlay = %s\n", cfg.video_overlay ? "on" : "off");
	if (cfg.mac_present)
		EMIT("mac = %02x:%02x:%02x:%02x:%02x:%02x\n",
			cfg.mac[0], cfg.mac[1], cfg.mac[2], cfg.mac[3],
			cfg.mac[4], cfg.mac[5]);
	if (cfg.hdf_present)
		EMIT("hdf = %s\n", cfg.hdf_path + 3);

#undef EMIT
	return off;
}


/* ---- resumable writer (the non-blocking save machine's steps) ----
 *
 * The atomic temp-then-replace sequence is split into one-FatFs-call
 * steps the caller drives from its service loop. Normal SD traffic
 * therefore interleaves with Zorro service instead of holding the
 * mailbox dispatch across the whole save. */

#define ZZ_CFG_SAVE_CHUNK 512u /* ~one sector of text per WRITE step */

static FIL cfg_step_file;
static const char *cfg_step_text;
static unsigned cfg_step_len;
static unsigned cfg_step_pos;
static int cfg_step_file_open;
static int cfg_step_active;
static int cfg_step_backup_moved;

int zz_config_save_begin(const char *text, unsigned len) {
	if (cfg_step_active) return -1;
	if (len == 0 || len >= ZZ_CONFIG_MAX_SIZE) return -1;
	memset(&cfg_step_file, 0, sizeof(cfg_step_file));
	cfg_step_text = text;
	cfg_step_len = len;
	cfg_step_pos = 0;
	cfg_step_file_open = 0;
	cfg_step_active = 1;
	return 0;
}

int zz_config_save_op(enum zz_config_save_op op) {
	FRESULT fr;
	UINT nwritten = 0;

	if (!cfg_step_active) return -1;
	switch (op) {
	case ZZ_CFG_SAVE_RECOVER_BAK:
		if (!cfg_step_backup_moved)
			return 1;
		fr = f_rename(ZZ_CONFIG_BAK_PATH, ZZ_CONFIG_FILE_PATH);
		if (fr != FR_OK && fr != FR_NO_FILE)
			return -1;
		cfg_step_backup_moved = 0;
		return 1;
	case ZZ_CFG_SAVE_UNLINK_TEMP:
		/* A partial temp from an interrupted save is junk; drop it
		 * so the create-always open starts clean. */
		fr = f_unlink(ZZ_CONFIG_TEMP_PATH);
		if (fr != FR_OK && fr != FR_NO_FILE) {
			printf("[CFG] save: unlink stale %s failed: %d\n",
				ZZ_CONFIG_TEMP_PATH, (int)fr);
			return -1;
		}
		return 1;
	case ZZ_CFG_SAVE_OPEN:
		fr = f_open(&cfg_step_file, ZZ_CONFIG_TEMP_PATH,
			FA_CREATE_ALWAYS | FA_WRITE);
		if (fr != FR_OK) {
			printf("[CFG] save: open %s failed: %d\n",
				ZZ_CONFIG_TEMP_PATH, (int)fr);
			return -1;
		}
		cfg_save_temp_pending = 1;
		cfg_step_file_open = 1;
		/* The text is CPU-written cacheable DDR; clean it before
		 * f_write so any direct-SD DMA path inside FatFs sees the
		 * fresh bytes. */
		Xil_DCacheFlushRange((UINTPTR)cfg_step_text, cfg_step_len);
		return 1;
	case ZZ_CFG_SAVE_WRITE: {
		unsigned chunk = cfg_step_len - cfg_step_pos;

		if (chunk > ZZ_CFG_SAVE_CHUNK) chunk = ZZ_CFG_SAVE_CHUNK;
		if (chunk == 0) return 1;
		fr = f_write(&cfg_step_file, cfg_step_text + cfg_step_pos,
			chunk, &nwritten);
		cfg_step_pos += nwritten;
		if (fr != FR_OK || nwritten != chunk) {
			printf("[CFG] save: write %s failed: %d (%u/%u)\n",
				ZZ_CONFIG_TEMP_PATH, (int)fr,
				cfg_step_pos, cfg_step_len);
			return -1;
		}
		return cfg_step_pos >= cfg_step_len;
	}
	case ZZ_CFG_SAVE_SYNC:
		if (f_sync(&cfg_step_file) != FR_OK) {
			printf("[CFG] save: sync %s failed\n",
				ZZ_CONFIG_TEMP_PATH);
			return -1;
		}
		return 1;
	case ZZ_CFG_SAVE_CLOSE:
		fr = f_close(&cfg_step_file);
		cfg_step_file_open = 0;
		if (fr != FR_OK) {
			printf("[CFG] save: close %s failed\n",
				ZZ_CONFIG_TEMP_PATH);
			return -1;
		}
		return 1;
	case ZZ_CFG_SAVE_UNLINK_BAK:
		fr = f_unlink(ZZ_CONFIG_BAK_PATH);
		if (fr != FR_OK && fr != FR_NO_FILE) {
			printf("[CFG] save: unlink stale %s failed: %d\n",
				ZZ_CONFIG_BAK_PATH, (int)fr);
			return -1;
		}
		return 1;
	case ZZ_CFG_SAVE_RENAME_BAK:
		fr = f_rename(ZZ_CONFIG_FILE_PATH, ZZ_CONFIG_BAK_PATH);
		if (fr != FR_OK && fr != FR_NO_FILE) {
			printf("[CFG] save: backup rename failed: %d\n",
				(int)fr);
			return -1;
		}
		if (fr == FR_OK)
			cfg_step_backup_moved = 1;
		return 1;
	case ZZ_CFG_SAVE_RENAME_LIVE:
		fr = f_rename(ZZ_CONFIG_TEMP_PATH, ZZ_CONFIG_FILE_PATH);
		if (fr != FR_OK) {
			printf("[CFG] save: commit rename failed: %d\n",
				(int)fr);
			return -1;
		}
		cfg_save_temp_pending = 0;
		cfg_step_backup_moved = 0;
		return 1;
	case ZZ_CFG_SAVE_RESTORE_BAK:
		/* Best-effort single attempt (the sync path's rule): a
		 * failure leaves ZZ9000.CFG absent until the next save. */
		fr = f_rename(ZZ_CONFIG_BAK_PATH, ZZ_CONFIG_FILE_PATH);
		if (fr != FR_OK && fr != FR_NO_FILE) {
			printf("[CFG] save: restore from " ZZ_CONFIG_BAK_FILENAME
				" failed: %d (" ZZ_CONFIG_FILENAME
				" unavailable until the next save)\n",
				(int)fr);
			return -1;
		}
		cfg_step_backup_moved = 0;
		return 1;
	default:
		return -1;
	}
}

void zz_config_save_end(void) {
	if (cfg_step_active && cfg_step_file_open) {
		(void)f_close(&cfg_step_file);
		cfg_step_file_open = 0;
	}
	cfg_step_active = 0;
	cfg_step_text = NULL;
	cfg_step_len = 0;
	cfg_step_pos = 0;
}

void zz_config_save_reset(void) {
	FRESULT fr;

	if (!cfg_step_active && !cfg_save_temp_pending &&
			!cfg_step_backup_moved)
		return;
	/* Release an interrupted writer first. If the atomic replace had
	 * already moved the live file aside, restore it before dropping
	 * the new temp snapshot. */
	zz_config_save_end();
	if (cfg_step_backup_moved) {
		fr = f_rename(ZZ_CONFIG_BAK_PATH, ZZ_CONFIG_FILE_PATH);
		if (fr == FR_OK || fr == FR_NO_FILE) {
			cfg_step_backup_moved = 0;
		} else {
			printf("[CFG] reset: restore from "
				ZZ_CONFIG_BAK_FILENAME " failed: %d\n", (int)fr);
		}
	}
	if (!cfg_save_temp_pending) return;
	cfg_save_temp_pending = 0;
	f_unlink(ZZ_CONFIG_TEMP_PATH);
}
