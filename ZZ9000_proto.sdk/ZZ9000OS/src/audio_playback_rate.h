/*
 * ZZ9000AX AHI per-period playback rate helpers.
 *
 * The legacy per-period path derives the source rate from the per-period
 * buffer size the Amiga mixer programs (frames per 20 ms period x 50).
 * This helper keeps that derivation host-testable so the production
 * symbol and the tested symbol cannot drift.
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

#endif /* ZZ_AUDIO_PLAYBACK_RATE_H */
