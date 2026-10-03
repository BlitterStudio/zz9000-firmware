/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef ZZ_OVERLAY_HW_H
#define ZZ_OVERLAY_HW_H

#include <stdint.h>

int overlay_hw_supported(void);
/* 1 on the current-bitstream image, 0 on the legacy-bitstream ELF.
 * REG_ZZ_VIDEOCAP_STATS exists only in RTL paired with the current image. */
int videocap_stats_hw_present(void);
void overlay_hw_stop(void);
int overlay_hw_start(uint32_t src_addr, uint32_t src_pitch,
                     uint16_t width, uint16_t height,
                     int16_t dst_x, int16_t dst_y,
                     uint8_t variant, uint32_t key_rgb,
                     uint8_t key_enabled, uint32_t generation);
int overlay_hw_start_scaled(uint32_t src_addr, uint32_t src_pitch,
                            uint16_t src_width, uint16_t src_height,
                            int16_t dst_x, int16_t dst_y,
                            uint16_t dst_width, uint16_t dst_height,
                            uint8_t variant, uint32_t key_rgb,
                            uint8_t key_enabled, uint32_t generation);
void overlay_hw_set_buffer(uint32_t src_addr, uint32_t generation);

#endif
