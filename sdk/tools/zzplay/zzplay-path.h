/* Presentation-path reporting for zzplay (R10).
 *
 * The overlay chooses native FPGA scanout versus the card-local ARM shadow
 * compositor inside firmware, so the player can only report the path it
 * actually got by asking ZZ9K_MEDIA_STATUS_PRESENTATION. Firmware without
 * that page answers BAD_REQUEST; that is the capability gate, and it maps to
 * ZZPLAY_PATH_UNKNOWN rather than to a playback failure.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef ZZPLAY_PATH_H
#define ZZPLAY_PATH_H

#include <stdint.h>

typedef enum ZZPlayPresentPath {
  /* Firmware predates the page, or the query failed: say so, never guess. */
  ZZPLAY_PATH_UNKNOWN = 0,
  /* Overlay not configured or not yet showing anything. */
  ZZPLAY_PATH_INACTIVE,
  /* Exact-size, fully visible, on the native FPGA overlay plane. */
  ZZPLAY_PATH_NATIVE_1_1,
  /* Resized or clipped, still handled by the native overlay's scaler. */
  ZZPLAY_PATH_NATIVE_SCALED,
  /* Card-local ARM RGB shadow compositor fallback. */
  ZZPLAY_PATH_SOFTWARE
} ZZPlayPresentPath;

typedef struct ZZPlayPresentInfo {
  ZZPlayPresentPath path;
  uint16_t src_w;
  uint16_t src_h;
  int16_t dst_x;
  int16_t dst_y;
  uint16_t dst_w;
  uint16_t dst_h;
  uint16_t screen_w;
  uint16_t screen_h;
  int clipped;
  int owned;
} ZZPlayPresentInfo;

/* Classify an already-decoded presentation status reply. */
void zzplay_present_classify(uint32_t flags, const uint64_t *value,
                             ZZPlayPresentInfo *out);

/* Classify from a zz9k_media_session_status() return code. Any non-OK status
 * yields ZZPLAY_PATH_UNKNOWN with zeroed geometry. */
void zzplay_present_from_status(int status, uint32_t flags,
                                const uint64_t *value,
                                ZZPlayPresentInfo *out);

/* Stable short name for display and logs. */
const char *zzplay_present_path_name(ZZPlayPresentPath path);

/* True when the two reports differ in a way worth telling the user about. */
int zzplay_present_changed(const ZZPlayPresentInfo *previous,
                           const ZZPlayPresentInfo *current);

#endif /* ZZPLAY_PATH_H */
