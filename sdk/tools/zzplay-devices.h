/* Audio output discovery for the settings window.
 *
 * AmigaOS-only. Nothing here opens an output for playback or claims
 * hardware: AHI is opened with AHI_NO_UNIT only to name audio modes, and
 * MHI drivers are listed by file name without being opened (opening an MHI
 * driver can initialise or claim its card).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef ZZPLAY_DEVICES_H
#define ZZPLAY_DEVICES_H

#include <stdint.h>

#include "zzplay-prefs.h"

#define ZZPLAY_DEVICES_NAME_MAX 64U
#define ZZPLAY_DEVICES_MAX_MHI 32U

/* One label per ahi.device unit 0..3: "Unit 0: <audio mode name>", or
 * "Unit 0: (not configured)" when ENV:Sys/ahi.prefs has no entry, or
 * "Unit 0" alone when AHI or its prefs are unavailable. */
void zzplay_devices_ahi_units(
    char labels[ZZPLAY_PREFS_AHI_UNITS][ZZPLAY_DEVICES_NAME_MAX]);

/* File names of LIBS:MHI/#?.library, sorted case-insensitively. Returns the
 * count (0 when the drawer is missing). */
uint32_t zzplay_devices_mhi_drivers(
    char names[][ZZPLAY_PREFS_DRIVER_MAX], uint32_t max);

#endif /* ZZPLAY_DEVICES_H */
