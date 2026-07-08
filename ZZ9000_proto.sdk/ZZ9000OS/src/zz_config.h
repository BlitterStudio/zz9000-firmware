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
	ZZ_CONFIG_KEY_NUM
};

struct zz_config {
	uint8_t loaded;                 /* file found and parsed */

	uint8_t videocap_mode_present;
	uint16_t videocap_mode;         /* enum zz_video_modes */

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
};

/* Mount the FAT volume, read and parse ZZ9000.CFG, unmount. Returns 0
 * if the file was found and parsed, -1 otherwise (defaults remain). */
int zz_config_load(void);

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

#endif
