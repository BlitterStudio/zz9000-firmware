/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef ZZAPP_MOCK_API_H
#define ZZAPP_MOCK_API_H

#include <stdint.h>
#include "zzapp.h"

/* Initializes a table against window. Caller may replace any slot with real code. */
void zzapp_mock_api_init(zzapp_api *api, uint8_t *window);
void zzapp_mock_time_set(uint64_t usec);
void zzapp_mock_time_advance(uint64_t usec);

#endif
