/*
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ZZ9K_MP3_BACKEND_CONFIG_H
#define ZZ9K_MP3_BACKEND_CONFIG_H

#define ZZ9K_MP3_BACKEND_MINIMP3 1

/*
 * The SDK audio service only promises MP3 decode. Keep the firmware build small
 * and avoid pulling in stdio-based helpers; streaming uses explicit callbacks.
 */
#define MINIMP3_ONLY_MP3 1
#define MINIMP3_NO_STDIO 1

#endif
