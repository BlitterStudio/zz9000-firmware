/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef ZZ_OVERLAY_HW_H
#define ZZ_OVERLAY_HW_H

#include <stdint.h>

void overlay_hw_stop(void);
int overlay_hw_start(uint32_t src_addr, uint32_t src_pitch,
                     uint16_t width, uint16_t height,
                     int16_t dst_x, int16_t dst_y,
                     uint8_t variant, uint32_t key_rgb,
                     uint8_t key_enabled, uint32_t generation);
void overlay_hw_set_buffer(uint32_t src_addr, uint32_t generation);

#endif
