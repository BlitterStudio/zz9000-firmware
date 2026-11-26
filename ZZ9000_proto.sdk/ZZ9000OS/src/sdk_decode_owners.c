/*
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 *
 * Card-pool owners of the decode backends and their core-1 reset reclaim.
 * The owner map is in sdk_compression.h. This unit needs only the card pool
 * and the app records, so the host tests drive the production reclaim.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdlib.h>
#include <string.h>

#include "sdk_compression.h"
#include "sdk_decode_reclaim.h"
#include "card_pool.h"
#include "zzapp_rec.h"
#include "lzh/zz9k_lzh.h"

typedef char decode_owner_is_media[
	SDK_DECODE_POOL_OWNER ==
	CARD_POOL_OWNER(CARD_POOL_CLASS_MEDIA, 0x00fffffeu) ? 1 : -1];
typedef char lease_decode_owner_is_media[
	SDK_AUDIO_LEASE_DECODE_OWNER ==
	CARD_POOL_OWNER(CARD_POOL_CLASS_MEDIA, 0x00fffffdu) ? 1 : -1];

/* App owners live at or above SDK_MEDIA_APP_INSTANCE_BASE. Video session
 * pool owners stay below that line, including session 4. An app's owner is
 * derived from the admission serial of its record (zzapp_rec.h), so it is
 * the same for the whole life of the record and unique among live ones. */
uint32_t sdk_compression_app_decode_owner(uint32_t pid)
{
	const zzapp_rec_t *rec = zzapp_rec_by_pid(pid);

	return rec && rec->hdr.owner_serial ?
		sdk_compression_app_owner_bits(rec->hdr.owner_serial) : 0U;
}

/*
 * Reclaim the blocks a killed core-1 decode still held. This closes the leak
 * and, together with the tracking table's clean/invalidate discipline, the
 * table-driven double-free.
 *
 * Residual (known, low-probability): the blocks live in the shared newlib heap,
 * and free() trusts that heap's chunk/arena metadata. That metadata was written
 * by core 1 under g_malloc_lock (ordering barriers, not write-back); if a line
 * of it were still dirty-only in core 1's L1 at the force-reset it would be
 * dropped, and freeing against the stale DRAM view could corrupt the free list.
 * Decode working sets are large, so that metadata is almost always evicted to
 * DRAM long before any reset -- but the only absolute guarantee is a dedicated
 * non-shared decode arena (tracked follow-up). This is a property of core-1
 * heap use under force-reset, not of the reclaim itself.
 *
 * Only core-1 owners are released here: the shared decode heap and the apps'
 * decoders. The core-0 audio ring decoders (SDK_AUDIO_LEASE_DECODE_OWNER)
 * survive a core-1 reset with their lease.
 */
unsigned sdk_compression_reclaim_app_decode(uint32_t owner)
{
	return card_pool_release_owner(&card_pool, owner) != 0U;
}

unsigned sdk_compression_reclaim_core1_decode(void)
{
	unsigned reclaimed;
	uint32_t pid;

	reclaimed = card_pool_release_owner(&card_pool,
					    SDK_DECODE_POOL_OWNER) != 0U;
	/* Core 1 was reset: every record that is not FREE may hold decode
	 * blocks under its owner. Its owner comes from its admission serial,
	 * so nothing besides the record has to remember it. */
	for (pid = 0U; pid < zzapp_rec_capacity(); ++pid) {
		const zzapp_rec_t *rec = zzapp_rec_by_pid(pid);

		if (zzapp_rec_state(pid) != ZZAPP_REC_FREE && rec->hdr.owner_serial)
			reclaimed += sdk_compression_reclaim_app_decode(
				sdk_compression_app_owner_bits(
					rec->hdr.owner_serial));
	}
	sdk_decode_invalidate_table();
	reclaimed += sdk_decode_reclaim(free);
	sdk_decode_flush_table();
	zz9k_lzh_reclaim();
	return reclaimed;
}
