/*
 * ZZ9000 SD Card Configuration File (ZZ9000.CFG)
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Reads an INI-style config file from the root of the SD card's FAT
 * volume (next to BOOT.bin) once at cold boot, before video/ethernet
 * bring-up. Settings that used to require ENV: variables set by the
 * Amiga drivers after system boot — or separate firmware builds like
 * the ns-pal flavor — apply immediately from power-on. See issue #33.
 *
 * Amiga-side drivers can query parsed values through REG_ZZ_CONFIG_KEY
 * (write a zz_config_key id, then read back value/present halves), so
 * options that only the drivers can act on (e.g. INT2 mode) can move
 * out of ENV: variables too.
 */

#ifndef ZZ_CONFIG_H
#define ZZ_CONFIG_H

#include <stdint.h>

#define ZZ_CONFIG_FILENAME  "ZZ9000.CFG"
#define ZZ_CONFIG_MAX_SIZE  4096
#define ZZ_CONFIG_HDF_NAME_MAX 63

/* Key ids for the REG_ZZ_CONFIG_KEY query interface. Shared by
 * convention with the Amiga drivers (they vendor their own copy). */
enum zz_config_key {
	ZZ_CONFIG_KEY_LOADED          = 0,  /* 1 if ZZ9000.CFG was found and parsed */
	ZZ_CONFIG_KEY_VIDEOCAP_MODE   = 1,  /* enum zz_video_modes value */
	ZZ_CONFIG_KEY_NS_VSYNC        = 2,  /* 0=off 1=pal 2=ntsc */
	ZZ_CONFIG_KEY_SCANLINE_MODE   = 3,  /* 0-3 */
	ZZ_CONFIG_KEY_SCANLINE_PARITY = 4,  /* 0-1 */
	ZZ_CONFIG_KEY_INT2            = 5,  /* 0=INT6 (default) 1=INT2 */
	ZZ_CONFIG_KEY_MAC_HI          = 6,  /* mac[0]<<8 | mac[1] */
	ZZ_CONFIG_KEY_MAC_MID         = 7,  /* mac[2]<<8 | mac[3] */
	ZZ_CONFIG_KEY_MAC_LO          = 8,  /* mac[4]<<8 | mac[5] */
	ZZ_CONFIG_KEY_OFFSCREEN_BITMAPS = 9, /* 0=off 1=on, informational (drivers query it) */
	/* Slot 10 was yuv_rect, removed in v2.8: nothing ever consumed it -
	 * ZZ_WriteYUVRect is an unconditional pass-through. The number stays
	 * reserved because these ids are a numeric ABI shared with drivers in
	 * the field; renumbering VIDEO_OVERLAY would silently misdirect an
	 * older ZZ9000.card. Querying it now reports absent, which is what an
	 * unset key has always meant. */
	ZZ_CONFIG_KEY_RESERVED_10     = 10,
	ZZ_CONFIG_KEY_VIDEO_OVERLAY   = 11, /* 0=off 1=on, informational (drivers query it) */
	ZZ_CONFIG_KEY_VIDEOCAP_SAMPLE = 12, /* 0=average 1=even 2=odd */
	ZZ_CONFIG_KEY_VIDEOCAP_SHRES  = 13, /* 0=filter 1=full */
	ZZ_CONFIG_KEY_VIDEOCAP_CROP_H = 14, /* 28 MHz samples */
	ZZ_CONFIG_KEY_VIDEOCAP_CROP_V = 15, /* captured lines */
	/* 1 when the loaded file exceeded the 4 KiB parse budget, so keys
	 * past the first ZZ_CONFIG_MAX_SIZE-1 bytes were ignored (the
	 * audio block is written last, so it is the first casualty).
	 * Slots from 16 up are the audio control plane (plan U5, KTD4);
	 * slot 10 above stays permanently reserved. */
	ZZ_CONFIG_KEY_AUDIO_TRUNCATED = 16,
	ZZ_CONFIG_KEY_NUM
};

/* Output identity is deliberately separate from the legacy mode/width/vsync
 * tuple. CENTERED projects to the full_60 tuple for compatibility, but must
 * still cause a distinct output-mode application in the video ISR. */
enum zz_videocap_output_profile {
	ZZ_VIDEOCAP_OUTPUT_FULL_60 = 0,
	ZZ_VIDEOCAP_OUTPUT_CENTERED_1080P_60 = 1,
};

struct zz_config {
	uint8_t loaded;                 /* file found and parsed */
	uint8_t videocap_output_profile; /* enum zz_videocap_output_profile */

	uint8_t videocap_mode_present;
	uint16_t videocap_mode;         /* enum zz_video_modes */
	uint8_t videocap_sample_present;
	uint16_t videocap_sample;       /* 0=average 1=even 2=odd */
	uint8_t videocap_shres_present;
	uint16_t videocap_shres;        /* 0=filter 1=full */
	uint8_t videocap_crop_h_present;
	uint16_t videocap_crop_h;       /* 0-4095, 28 MHz samples */
	uint8_t videocap_crop_v_present;
	uint16_t videocap_crop_v;       /* 0-4095, captured lines */

	uint8_t ns_vsync_present;
	uint16_t ns_vsync;              /* 0=off 1=pal 2=ntsc */

	uint8_t scanline_mode_present;
	uint16_t scanline_mode;         /* 0-3, video_formatter scanline_width */

	uint8_t scanline_parity_present;
	uint16_t scanline_parity;       /* 0-1 */

	uint8_t int2_present;
	uint16_t int2;                  /* 0-1, informational (drivers query it) */

	uint8_t mac_present;
	uint8_t mac[6];
	uint8_t hdf_present;
	char hdf_path[ZZ_CONFIG_HDF_NAME_MAX + 4]; /* "0:/" + name + NUL */

	uint8_t offscreen_bitmaps_present;
	uint16_t offscreen_bitmaps;     /* 0-1, informational (drivers query it) */

	uint8_t video_overlay_present;
	uint16_t video_overlay;         /* 0-1, informational (drivers query it) */

	/* Audio control-plane keys (plan U5, KTD4), parsed here and folded
	 * into the scene module at boot by audio_scene_load_config().
	 * Absent or out-of-range keys leave the module's built-in
	 * defaults. The decimal-only grammar caps each value at 0xffff, so
	 * scenes serialize as grouped keys: EQ band pairs pack as
	 * hi*128+lo (each band 0..100), audio_scene_out packs
	 * prefactor*128+volume, and audio_baseline packs paula<<8|ax.
	 * audio_scene_mask bit N is the presence of key field N: bits
	 * 0..7 are lpf/eq01..eq89/out/pan and bits 8..15 the name chunks
	 * nm1..nm8 (each packs two ASCII label chars as c1*256+c2, 0 =
	 * terminator). */
#define ZZ_CFG_AUDIO_SCENES 8
#define ZZ_CFG_AUDIO_NAME_CHUNKS 8

	uint8_t audio_active_present;
	uint16_t audio_active;          /* 0..7 */
	uint8_t audio_baseline_present;
	uint16_t audio_baseline;        /* paula<<8 | ax, legs 0..255 */
	uint16_t audio_scene_mask[ZZ_CFG_AUDIO_SCENES];  /* bit per key */
	uint16_t audio_scene_lpf[ZZ_CFG_AUDIO_SCENES];   /* 1..23900 Hz */
	uint16_t audio_scene_eq[ZZ_CFG_AUDIO_SCENES][5]; /* band pairs */
	uint16_t audio_scene_out[ZZ_CFG_AUDIO_SCENES];   /* pref*128+vol */
	uint16_t audio_scene_pan[ZZ_CFG_AUDIO_SCENES];   /* 0..100 */
	uint16_t audio_scene_nm[ZZ_CFG_AUDIO_SCENES][ZZ_CFG_AUDIO_NAME_CHUNKS];
	                                /* label chunks c1*256+c2 */
	uint8_t truncated;              /* file exceeded the parse budget */
};

enum zz_config_file_status {
	ZZ_CONFIG_FILE_OK       = 0,
	ZZ_CONFIG_FILE_NO_FILE  = 1,
	ZZ_CONFIG_FILE_IO_ERROR = 2,
	ZZ_CONFIG_FILE_IDLE     = 0xFFFF,
};

/* Mount the FAT volume, read and parse ZZ9000.CFG, unmount. Returns 0
 * if the file was found and parsed, -1 otherwise (defaults remain). */
int zz_config_load(void);

/* Read the current raw ZZ9000.CFG contents into `buffer` (up to
 * max_len bytes; the tail of an oversized file is ignored, matching
 * what the boot-time parser sees). Uses the already-registered FAT
 * volume — never call before sd_storage_init() has mounted it.
 * Returns a zz_config_file_status; *out_len is the byte count staged
 * (0 unless ZZ_CONFIG_FILE_OK). */
uint16_t zz_config_read_raw(void *buffer, uint32_t max_len, uint32_t *out_len);

/* Parse `len` bytes of config text into the global config. Pure —
 * exercised directly by the host unit tests. Returns the number of
 * key lines accepted. */
int zz_config_parse(const char *text, unsigned len);

/* Reset the global config to all-absent defaults. */
void zz_config_reset(void);

const struct zz_config* zz_config_get(void);

/* Register query backend: value of `key`, with *present set to 1 if
 * the key was given in the config file (for ZZ_CONFIG_KEY_LOADED,
 * whether the file was loaded). Unknown keys read as 0/absent. */
uint16_t zz_config_query(uint16_t key, uint16_t *present);

/* Regenerate the non-audio keys of ZZ9000.CFG from parsed state (the
 * U5 writer content policy: present keys only, the atomic
 * videocap_profile form, comments not preserved). Appends a
 * NUL-terminated text at buf[off] and returns the new length, or -1
 * when it does not fit. */
int zz_config_emit_present_keys(char *buf, unsigned size, int off);

/* Atomically replace ZZ9000.CFG with `len` bytes of text (U5, KTD5):
 * write 0:/ZZCFG.TMP, sync and close, keep the previous file as
 * 0:/ZZ9000.BAK, then rename onto ZZ9000.CFG — fw_update's commit
 * discipline. Any failure leaves the original intact and reports -1;
 * a partial temp file stays behind for the reset hook / next save. */
int zz_config_save_file(const char *text, unsigned len);

/* Amiga-reset hook, the CFG counterpart of fw_update_reset(): a save
 * interrupted between open and commit never touched the original —
 * drop its junk temp file. */
void zz_config_save_reset(void);

#endif
