/*
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 *
 * Single-writer allocation tracker for the core-1 one-shot decode path.
 *
 * The dual-core scheduler runs one-shot decompression on core 1. Each decode
 * makes heap allocations (zlib inflateInit2 state + window; LZMA probs + dict)
 * that are only released when the backend's cleanup (inflateEnd / LzmaDec_Free)
 * runs on normal return. If core 1 is cold-reset mid-decode -- a mailbox
 * teardown that outlives the quiesce budget, or a fault -- that cleanup never
 * runs and those blocks leak until a full firmware reboot.
 *
 * The core-1 worker records every live allocation here; core 0 frees any
 * survivors at the cold-restart choke point, while core 1 is provably halted.
 *
 * Concurrency: only core 1 writes the table (the allocator wrappers gate on
 * smp_cpu_id()), and core 0 reads/reclaims it only when core 1 is halted, so
 * there is never concurrent access. Each slot is a single pointer-sized word,
 * so a store is atomic on ARM: a worker halted mid-update leaves the table
 * self-consistent. The wrapper ordering (track AFTER malloc, untrack BEFORE
 * free) means an interrupted worker leaks at most one block and never lets
 * core 0 double-free.
 *
 * Cache: a reset core's dirty L1 lines are dropped, not snooped, so the
 * firmware wrappers maintain this table to the point of coherency explicitly
 * (core 1 cleans after each track/untrack; core 0 invalidates before reading
 * and flushes after clearing) using sdk_decode_table_base()/_bytes(). The table
 * is cache-line aligned and line-sized so those range operations touch nothing
 * else.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef SDK_DECODE_RECLAIM_H
#define SDK_DECODE_RECLAIM_H

/*
 * Maximum distinct live allocations a single core-1 decode holds at once.
 * zlib inflate uses 2 (state + window); LZMA/LZMA2 use 2-3 (probs + dict).
 * 16 is a safe ceiling with generous headroom.
 */
#define SDK_DECODE_MAX_TRACKED 16u

/* Record a live allocation. Call AFTER malloc succeeds. NULL is ignored. */
void sdk_decode_track(void *ptr);

/* Drop a tracked allocation. Call BEFORE free. NULL / untracked is ignored. */
void sdk_decode_untrack(void *ptr);

/*
 * Free every still-tracked allocation via free_fn and clear the table. Call on
 * core 0 while core 1 is halted. Returns the number of blocks reclaimed (0 when
 * the last decode completed cleanly).
 */
unsigned sdk_decode_reclaim(void (*free_fn)(void *));

/* Number of slots currently occupied. For tests and diagnostics. */
unsigned sdk_decode_tracked_count(void);

/*
 * Extent of the tracking table, for the firmware's cache clean/invalidate range
 * (the table is cache-line aligned and line-sized). base is never NULL.
 */
void *sdk_decode_table_base(void);
unsigned sdk_decode_table_bytes(void);

/*
 * Cortex-A9 cache maintenance (sdk_decode_reclaim_arm.c, ARM-only). Core 1
 * cleans the table to DRAM after each track/untrack; core 0 invalidates its
 * stale copy before reading at reclaim. Not built for host tests, which never
 * call them.
 */
void sdk_decode_flush_table(void);
void sdk_decode_invalidate_table(void);

#endif /* SDK_DECODE_RECLAIM_H */
