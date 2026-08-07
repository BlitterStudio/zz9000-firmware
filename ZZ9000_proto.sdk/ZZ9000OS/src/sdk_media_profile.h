/*
 * MNT ZZ9000 Amiga Graphics and ARM Coprocessor Card Operating System
 * (ZZ9000OS)
 *
 * Per-stage timing for the media pipeline (U7).
 *
 * U7 requires optimisation decisions to come from measurement, not
 * assumption - in particular whether the planar-to-YUY2 pack is a material
 * cost compared with decode, since that is what gates the planar FPGA
 * subproject (R13). The host only sees one lumped "SDK decode" figure, so
 * the split has to be measured on the card.
 *
 * Concurrency: every accumulator has exactly ONE writer core - the decode
 * and pack stages run on core 1, present runs on core 0 - and the block
 * lives in the SCU-coherent task-queue region, so aligned 32-bit updates
 * need no lock and no cache maintenance. Readers are core 0 only.
 *
 * The microsecond accumulators are 32-bit and wrap after roughly 71 minutes
 * of accumulated time *in a single stage*. That is far beyond any profiling
 * run, and the per-frame averages the tooling reports are unaffected by an
 * eventual wrap of the raw total.
 *
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef SDK_MEDIA_PROFILE_H
#define SDK_MEDIA_PROFILE_H

#include <stdint.h>

enum {
	SDK_MEDIA_PROFILE_VIDEO_DECODE = 0,
	SDK_MEDIA_PROFILE_YUY2_PACK = 1,
	SDK_MEDIA_PROFILE_PRESENT = 2,
	SDK_MEDIA_PROFILE_AUDIO_DECODE = 3,
	SDK_MEDIA_PROFILE_STAGES = 4
};

struct SDKMediaProfileStage {
	volatile uint32_t microseconds;
	volatile uint32_t calls;
};

struct SDKMediaProfile {
	struct SDKMediaProfileStage stage[SDK_MEDIA_PROFILE_STAGES];
};

/* Monotonic microseconds from the global timer. Returns 0 when no timer is
 * available (host tests), which callers treat as "unmeasured". */
uint32_t sdk_media_profile_now_us(void);

/* Accumulate one completed stage. `start_us` comes from
 * sdk_media_profile_now_us(); a zero start is ignored so an unmeasured build
 * records nothing rather than nonsense. Safe from either core for its own
 * stage. */
void sdk_media_profile_record(uint32_t stage, uint32_t start_us);

/* Zeroed at session begin so each run measures only itself. */
void sdk_media_profile_reset(void);

/* Read one stage. Returns 0 for an out-of-range stage. */
int sdk_media_profile_read(uint32_t stage, uint32_t *microseconds,
                           uint32_t *calls);

#endif /* SDK_MEDIA_PROFILE_H */
