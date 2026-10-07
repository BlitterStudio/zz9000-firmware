/* Read the AHI unit table from ENV:Sys/ahi.prefs.
 *
 * ahi.device units 0..3 are each bound to an audio mode by the AHI
 * preferences editor. Showing that binding ("Unit 0: ZZ9000AX: HiFi 16 bit
 * stereo++") is what makes choosing a unit meaningful, so the settings
 * window parses the IFF FORM PREF file directly: big-endian IFF chunks,
 * each AHIU chunk a struct AHIUnitPrefs (devices/ahi.h):
 *
 *   UBYTE unit; UBYTE obsolete; UWORD channels; ULONG audio_mode;
 *   ULONG frequency; Fixed monitor_volume; Fixed input_gain;
 *   Fixed output_volume; ULONG input; ULONG output;
 *
 * Only the first 12 bytes are needed. Chunks are word-padded. Units other
 * than 0..3 (255 is the low-level "music unit") are ignored.
 *
 * Host-testable. SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef ZZPLAY_AHIPREFS_H
#define ZZPLAY_AHIPREFS_H

#include <stddef.h>
#include <stdint.h>

#define ZZPLAY_AHIPREFS_PATH "ENV:Sys/ahi.prefs"
#define ZZPLAY_AHIPREFS_UNITS 4U

typedef struct ZZPlayAHIUnitPref {
  uint8_t present;
  uint16_t channels;
  uint32_t audio_mode;      /* AHI audio mode ID */
  uint32_t frequency;
} ZZPlayAHIUnitPref;

/* Parse a complete ahi.prefs image. Returns the number of units 0..3 found,
 * or -1 when the data is not an IFF FORM PREF. Malformed or truncated
 * chunks end the scan without reading past `length`. */
int zzplay_ahiprefs_parse(const uint8_t *data, size_t length,
                          ZZPlayAHIUnitPref units[ZZPLAY_AHIPREFS_UNITS]);

#endif /* ZZPLAY_AHIPREFS_H */
