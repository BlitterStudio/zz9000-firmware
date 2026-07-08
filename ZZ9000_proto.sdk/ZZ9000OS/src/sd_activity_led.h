/*
 * ZZ9000 SD/HDF activity LED
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef SD_ACTIVITY_LED_H
#define SD_ACTIVITY_LED_H

#include "xtime_l.h"

#define SD_ACTIVITY_LED_MIO_PIN 10
#define SD_ACTIVITY_LED_STRETCH_TICKS (COUNTS_PER_SECOND / 10U)

int sd_activity_led_init(void);
void sd_activity_led_begin(void);
void sd_activity_led_end(void);
void sd_activity_led_poll(void);

#endif /* SD_ACTIVITY_LED_H */
