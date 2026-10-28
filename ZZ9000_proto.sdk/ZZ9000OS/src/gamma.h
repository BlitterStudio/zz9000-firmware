/*
 * Output gamma table for the ZZ9000 video formatter LUT.
 *
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The FPGA applies a 256-entry per-channel table to the final composed
 * pixel (video_formatter.v OP_GAMMA). The ARM computes one curve and
 * loads it into the red, green and blue tables alike.
 */

#ifndef ZZ_GAMMA_H
#define ZZ_GAMMA_H

#include <stdint.h>

/* Gamma is held in hundredths: 1.8 is 180. */
#define GAMMA_HUNDREDTHS_MIN 20     /* 0.20 */
#define GAMMA_HUNDREDTHS_MAX 500    /* 5.00 */
#define GAMMA_HUNDREDTHS_IDENTITY 100 /* 1.00 */

#define GAMMA_LUT_ENTRIES 256

/* Fills lut with out = round(255 * (in/255) ^ (1/gamma)); entry 0 stays 0
 * and entry 255 stays 255. Returns 0 and leaves lut untouched when the
 * gamma value is outside the accepted range. */
int gamma_table_build(uint8_t lut[GAMMA_LUT_ENTRIES], uint16_t gamma_hundredths);

#endif
