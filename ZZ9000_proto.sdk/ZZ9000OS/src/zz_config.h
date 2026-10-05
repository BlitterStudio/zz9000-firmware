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
/* Doubled for the ZZTop scandoubler-calibration keys (width/height ride
 * with the phase/crop block) so the eight saved audio scenes no longer
 * brush the truncation ceiling. Shared with the Amiga editor's
 * ZZCFG_MAX_SIZE. */
#define ZZ_CONFIG_MAX_SIZE  8192
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
	 * Slot 10 above stays permanently reserved. */
	ZZ_CONFIG_KEY_AUDIO_TRUNCATED = 16,
	ZZ_CONFIG_KEY_VIDEOCAP_PHASE  = 17, /* legacy E7M MMCM steps, -255..255 */
	ZZ_CONFIG_KEY_VIDEOCAP_C28_PHASE = 18, /* C28 MMCM steps, -896..895 */
	/* Manual capture-window bounds (ZZTop calibration): width in
	 * captured words (16-aligned, 256..1280), height in source lines
	 * (100..1024). 0/absent = automatic. The live runtime override is
	 * CARD_FEATURE_VIDEOCAP_GEOMETRY; these keys persist it. */
	ZZ_CONFIG_KEY_VIDEOCAP_WIDTH  = 26, /* captured words, 0=automatic */
	ZZ_CONFIG_KEY_VIDEOCAP_HEIGHT = 27, /* source lines, 0=automatic */
	/* Fail-closed Z3 Fast-RAM advertisement. FAST_RAM reads the saved
	 * preference; FAST_RAM_OUTCOME reads the effective boot decision
	 * (enum zz_fastram_outcome), so Amiga software distinguishes
	 * configured-on-but-withheld from enabled. */
	ZZ_CONFIG_KEY_FAST_RAM        = 19, /* 0=off 1=on */
	ZZ_CONFIG_KEY_FAST_RAM_OUTCOME = 20, /* enum zz_fastram_outcome */
	/* Runtime RTG VDMA geometry diagnostics (keys 21-25; NOT config
	 * file values — served from live video state so UART-less users
	 * can capture the scanout geometry during a transient display
	 * fault). LINE/STRIDE/PAN return the raw 16-bit values the last
	 * video_mode_init programmed; INFO packs [15:13] hdiv,
	 * [12:11] stride_div, [10:0] content hsize; MODESEL packs
	 * [15:10] colormode, [9:8] scalemode, [7:0] mode id. */
	ZZ_CONFIG_KEY_RTG_GEOM_LINE    = 21,
	ZZ_CONFIG_KEY_RTG_GEOM_STRIDE  = 22,
	ZZ_CONFIG_KEY_RTG_GEOM_PAN     = 23,
	ZZ_CONFIG_KEY_RTG_GEOM_INFO    = 24,
	ZZ_CONFIG_KEY_RTG_GEOM_MODESEL = 25,
	/* Runtime native-capture geometry transaction state. REQUEST is the
	 * override pair (zero means automatic); APPLIED is the exact successful
	 * native VDMA window in captured words and source rows. */
	ZZ_CONFIG_KEY_VCAP_GEOMETRY_REQUEST_WIDTH = 28,
	ZZ_CONFIG_KEY_VCAP_GEOMETRY_REQUEST_HEIGHT = 29,
	ZZ_CONFIG_KEY_VCAP_GEOMETRY_APPLIED_WIDTH = 30,
	ZZ_CONFIG_KEY_VCAP_GEOMETRY_APPLIED_HEIGHT = 31,
	ZZ_CONFIG_KEY_VCAP_GEOMETRY_REQUEST_SERIAL = 32,
	ZZ_CONFIG_KEY_VCAP_GEOMETRY_APPLIED_SERIAL = 33,
	ZZ_CONFIG_KEY_VCAP_GEOMETRY_STATUS = 34,
	ZZ_CONFIG_KEY_NUM
};

/* Effective boot decision for the Z3 Fast-RAM advertisement, set by
 * the boot gate path after the bounded CFG load. PENDING means the
 * decision has not run (pre-gate firmware reads it as absent). */
enum zz_fastram_outcome {
	ZZ_FASTRAM_OUTCOME_PENDING   = 0,
	ZZ_FASTRAM_OUTCOME_ENABLED   = 1, /* parsed `on`, advertised */
	ZZ_FASTRAM_OUTCOME_OFF       = 2, /* parsed `off` */
	ZZ_FASTRAM_OUTCOME_ABSENT    = 3, /* key absent from the file */
	ZZ_FASTRAM_OUTCOME_INVALID   = 4, /* malformed fast_ram line(s) */
	ZZ_FASTRAM_OUTCOME_TRUNCATED = 5, /* file over the parse budget */
	ZZ_FASTRAM_OUTCOME_MEDIA_ERR = 6, /* mount/open/read failed */
	ZZ_FASTRAM_OUTCOME_TIMEOUT   = 7, /* bounded load missed the deadline */
	ZZ_FASTRAM_OUTCOME_BAK_ON    = 8, /* enabled via ZZ9000.BAK recovery */
};

/* Output identity is deliberately separate from the legacy mode/width/vsync
 * tuple. The CENTERED profiles project to the full_60 tuple for
 * compatibility, but must still cause a distinct output-mode application in
 * the video ISR (60 Hz uses mode ZZVMODE_1920x1080_60, 50 Hz uses
 * ZZVMODE_1920x1080_50). The MATCH variant is the experimental source-locked
 * profile: same canvas/geometry and same legacy tuple, but the output
 * refresh follows the runtime-detected source standard (PAL selects mode
 * ZZVMODE_1920x1080_50, NTSC ZZVMODE_1920x1080_60) and requires the
 * source-sync controller in the loaded bitstream; without it the effective
 * profile degrades to CENTERED_1080P_60, never to free-running sync. */
enum zz_videocap_output_profile {
	ZZ_VIDEOCAP_OUTPUT_FULL_60 = 0,
	ZZ_VIDEOCAP_OUTPUT_CENTERED_1080P_60 = 1,
	ZZ_VIDEOCAP_OUTPUT_CENTERED_1080P_50 = 2,
	ZZ_VIDEOCAP_OUTPUT_CENTERED_1080P_MATCH = 3,
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
	uint8_t videocap_phase_present;
	int16_t videocap_phase;          /* legacy E7M fine-phase steps, -255..255 */
	uint8_t videocap_c28_phase_present;
	int16_t videocap_c28_phase;      /* C28 fine-phase steps, -896..895 */
	uint8_t videocap_width_present;
	uint16_t videocap_width;         /* capture window words, 0=automatic */
	uint8_t videocap_height_present;
	uint16_t videocap_height;        /* capture window lines, 0=automatic */

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
	/* hdf_present with an empty hdf_path is `hdf = off`: SD boot is
	 * disabled. Absent key means the default 0:/zz9000.hdf. */
	uint8_t hdf_present;
	char hdf_path[ZZ_CONFIG_HDF_NAME_MAX + 4]; /* "0:/" + name + NUL, or "" */

	uint8_t offscreen_bitmaps_present;
	uint16_t offscreen_bitmaps;     /* 0-1, informational (drivers query it) */

	uint8_t video_overlay_present;
	uint16_t video_overlay;         /* 0-1, informational (drivers query it) */

	uint8_t fast_ram_present;
	uint16_t fast_ram;              /* 0-1; advertisement is fail-closed */
	uint8_t fast_ram_invalid;       /* any malformed fast_ram line poisons */
	uint8_t fastram_outcome;        /* enum zz_fastram_outcome */

	/* Audio control-plane keys, parsed here and folded into the scene
	 * module at boot. Absent/out-of-range keys keep built-in defaults.
	 * audio_ceiling_paula/audio_ceiling_ax are per-card measured clean
	 * ceilings (1..4095). EQ pairs pack hi*128+lo, scene out packs
	 * prefactor*128+volume, and baseline packs paula<<8|ax.
	 * audio_scene_mask bit N is the presence of key field N: bits
	 * 0..7 are lpf/eq01..eq89/out/pan and bits 8..15 the name chunks
	 * nm1..nm8 (each packs two ASCII label chars as c1*256+c2, 0 =
	 * terminator). */
#define ZZ_CFG_AUDIO_SCENES 8
#define ZZ_CFG_AUDIO_NAME_CHUNKS 8

	uint8_t audio_active_present;
#define ZZ_CFG_AUDIO_CEILING_MIN 1
#define ZZ_CFG_AUDIO_CEILING_MAX 4095
	uint16_t audio_active;          /* 0..7 */
	uint8_t audio_baseline_present;
	uint16_t audio_baseline;        /* paula<<8 | ax, legs 0..255 */
	uint8_t audio_ceiling_paula_present;
	uint16_t audio_ceiling_paula;   /* measured clean ceiling, 1..4095 */
	uint8_t audio_ceiling_ax_present;
	uint16_t audio_ceiling_ax;      /* measured clean ceiling, 1..4095 */
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

/* Bounded boot-time variant for the Fast-Ram decision: arms the SD
 * deadline for `deadline_ms` (0 = unbounded vendor behavior) around
 * the same load, then records the effective Fast-Ram boot outcome
 * (read back through ZZ_CONFIG_KEY_FAST_RAM_OUTCOME). mount_volume
 * selects the cold-boot path (mount the card, unmount after); pass 0
 * when the FAT volume is already registered (the warm-reset reload
 * runs against sd_storage's live mount, which a private remount
 * would destroy). Same return convention as zz_config_load(). */
int zz_config_load_fastram(uint32_t deadline_ms, int mount_volume);

/* Boot deadline for the early CFG load, in milliseconds. Generous
 * against a healthy card (typical mount+read is far shorter) and
 * tuned from hardware qualification measurements. */
#define ZZ_CONFIG_FASTRAM_DEADLINE_MS 1000u

/* Effective advertisement decision from the recorded boot outcome:
 * true only for ENABLED and BAK_ON -- a TIMEOUT result reads false
 * even when the parsed preference was `on` (fail closed). */
int zz_config_fastram_advertise(void);

/* Diagnostics suppressed by the last bounded load (its summary line
 * reports the count instead of one UART line per skipped key). */
uint16_t zz_config_diag_count(void);

/* Warm-reset variant: same bounded load through the live volume and
 * the same outcome recording, but only the Fast-Ram fields of the
 * live configuration change -- every other key keeps its cold-boot
 * value so register queries and boot-applied settings do not shift
 * mid-session when the card was edited, removed, or unreadable. */
int zz_config_fastram_reload_warm(uint32_t deadline_ms);

/* Stable lowercase name for an outcome (boot summary line, logs). */
const char *zz_fastram_outcome_name(enum zz_fastram_outcome o);

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

/* Fail-closed Fast-RAM advertisement decision from parsed state: true
 * only for an un-truncated, un-poisoned, present `on`. Load failures
 * leave the fields cleared (reset runs first), so they read disabled
 * without checking `loaded`. */
int zz_config_fastram_enabled(void);


/* Regenerate the non-audio keys of ZZ9000.CFG from parsed state (the
 * U5 writer content policy: present keys only, the atomic
 * videocap_profile form, comments not preserved). Appends a
 * NUL-terminated text at buf[off] and returns the new length, or -1
 * when it does not fit. */
int zz_config_emit_present_keys(char *buf, unsigned size, int off);


/* ---- resumable writer steps (the non-blocking save machine) ---- */

/* The individual operations of the save above. Each zz_config_save_op()
 * call performs AT MOST ONE FatFs call, so a caller-owned state machine
 * (audio_scene.c's save machine) can interleave SD traffic with its
 * service loop; the FIL handle and the write cursor live here. */
enum zz_config_save_op {
	ZZ_CFG_SAVE_RECOVER_BAK = 0, /* restore an interrupted prior save */
	ZZ_CFG_SAVE_UNLINK_TEMP,     /* drop a stale ZZCFG.TMP */
	ZZ_CFG_SAVE_OPEN,            /* create ZZCFG.TMP */
	ZZ_CFG_SAVE_WRITE,           /* one <=512-byte chunk of the text */
	ZZ_CFG_SAVE_SYNC,
	ZZ_CFG_SAVE_CLOSE,
	ZZ_CFG_SAVE_UNLINK_BAK,      /* drop a stale ZZ9000.BAK */
	ZZ_CFG_SAVE_RENAME_BAK,      /* keep the original as ZZ9000.BAK */
	ZZ_CFG_SAVE_RENAME_LIVE,     /* commit: temp -> ZZ9000.CFG */
	ZZ_CFG_SAVE_RESTORE_BAK      /* failure path: ZZ9000.BAK back */
};

/* Stage `text` for a resumable save; the buffer must stay valid until
 * the sequence ends (zz_config_save_end). Returns 0, or -1 when a
 * sequence is already staged or the length is out of range. */
int zz_config_save_begin(const char *text, unsigned len);

/* Perform one operation of a begun sequence. Returns 1 when the
 * operation completed (for ZZ_CFG_SAVE_WRITE: the whole text is
 * written), 0 when WRITE has more chunks queued, and -1 on a FatFs
 * failure (same diagnostics as zz_config_save_file; the original is
 * never touched, the partial temp stays for the reset hook). */
int zz_config_save_op(enum zz_config_save_op op);

/* End a resumable sequence, settled or abandoned. Closes an open temp
 * handle before releasing the staged text. Safe to call when none is
 * active. */
void zz_config_save_end(void);

/* Amiga-reset hook, the CFG counterpart of fw_update_reset(): close an
 * interrupted writer, restore ZZ9000.BAK when live was already moved
 * aside, then drop the abandoned temp snapshot. */
void zz_config_save_reset(void);

#endif
