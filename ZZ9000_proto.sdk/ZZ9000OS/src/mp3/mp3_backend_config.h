/*
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ZZ9K_MP3_BACKEND_CONFIG_H
#define ZZ9K_MP3_BACKEND_CONFIG_H

#define ZZ9K_MP3_BACKEND_MINIMP3 1

/* The audio stream service backs both the MHI MP3 path and mpega.library.
 * mpega.library clients such as RiVA pass MPEG Layer II elementary audio, so
 * keep minimp3's Layer I/II decoder enabled as part of the compatibility
 * contract.  Streaming still uses explicit callbacks and needs no stdio. */
#define MINIMP3_NO_STDIO 1

#endif
