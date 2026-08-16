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

#include <stdio.h>
#include <string.h>
#include <ff.h>
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

/* Flat filename in the FAT volume root: no path separators or drive
 * prefixes, printable ASCII only. Mirrors fw_update's rules. */
static int hdf_name_valid(const char *s) {
	unsigned len = 0;
	if (!*s || *s == '.') return 0;
	for (; *s; s++, len++) {
		char c = *s;
		if (c == '/' || c == '\\' || c == ':' || c < 0x21 || c > 0x7e)
			return 0;
	}
	return len <= ZZ_CONFIG_HDF_NAME_MAX;
}

static int apply_key(const char *key, const char *value) {
	/* Preferred user-facing native-video setting.  It replaces three
	 * independently editable legacy keys with one coherent profile, while
	 * still filling the same internal fields so older driver/runtime paths
	 * keep their established behaviour.  Legacy keys remain accepted below;
	 * normal last-value-wins parsing therefore also works for mixed files. */
	if (token_eq(key, "videocap_profile")) {
		int output_profile = ZZ_VIDEOCAP_OUTPUT_FULL_60;

		if (token_eq(value, "centered_1080p_60")) {
			cfg.videocap_mode = ZZVMODE_800x600;
			cfg.videocap_shres = 1;
			cfg.ns_vsync = 0;
			output_profile = ZZ_VIDEOCAP_OUTPUT_CENTERED_1080P_60;
		} else if (token_eq(value, "full_60")) {
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

	fr = f_open(&f, "0:/" ZZ_CONFIG_FILENAME, FA_READ);
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

	/* The FAT volume is registered by sd_storage_init() at this point;
	 * f_open fails cleanly (FR_NOT_ENABLED) if no card is mounted. */
	fr = f_open(&f, "0:/" ZZ_CONFIG_FILENAME, FA_READ);
	if (fr == FR_NO_FILE || fr == FR_NO_PATH) {
		return ZZ_CONFIG_FILE_NO_FILE;
	}
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
	default:
		break;
	}

	if (present) *present = p;
	return p ? v : 0;
}
