/*
 * ZZ9000AX AHI per-period playback rate helpers.
 *
 * The legacy per-period path derives the source rate from the per-period
 * buffer size the Amiga mixer programs (frames per 20 ms period x 50).
 * These helpers keep that derivation and its reset rule host-testable:
 * a rate change (including the first call after a reset, where the last
 * rate reads 0) re-initializes the converter instance exactly once
 * before the first conversion at the new rate.
 *
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ZZ_AUDIO_PLAYBACK_RATE_H
#define ZZ_AUDIO_PLAYBACK_RATE_H

#include <stdint.h>

/* Frames per 20 ms period -> source rate in Hz. */
static inline uint32_t zz_audio_playback_rate(uint16_t buf_samples)
{
	return (uint32_t)buf_samples * 50U;
}

/* True when the converter must be re-initialized before converting this
 * period (rate change or first use after reset). 48 kHz never needs a
 * converter: it stays a byte-identical bypass. */
static inline int zz_audio_playback_rate_changed(uint32_t now,
                                                 uint32_t last)
{
	return now != last;
}

#endif /* ZZ_AUDIO_PLAYBACK_RATE_H */
