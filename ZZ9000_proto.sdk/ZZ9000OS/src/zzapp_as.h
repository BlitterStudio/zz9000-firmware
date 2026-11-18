/*
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ZZAPP_AS_H
#define ZZAPP_AS_H

#include <stdint.h>

#include "memorymap.h"

#define ZZAPP_AS_WINDOW_BASE 0xc0000000U
#define ZZAPP_AS_WINDOW_END  0xe0000000U
/*
 * Host-visible buffers live below the 1 MiB stack guard and the largest
 * permitted 1 MiB app stack at the top of the private window. The loader
 * must leave this range unmapped for private image, heap, and stack pages.
 */
#define ZZAPP_AS_ALIAS_BASE 0xdc000000U
#define ZZAPP_AS_ALIAS_END  0xdfe00000U
#define ZZAPP_AS_ALIAS_PAGE_COUNT \
	((ZZAPP_AS_ALIAS_END - ZZAPP_AS_ALIAS_BASE) / 4096U)
#define ZZAPP_AS_ENOMEM       (-3)
/* The L2 reserve of an address space could not be granted. */
#define ZZAPP_AS_ETABLES      (-4)

/* Address spaces are named by a process-record index (pid). */
#define ZZAPP_AS_MAX_APPS      1024U
#define ZZAPP_AS_NO_PID        0xffffffffU
#define ZZAPP_AS_L1_BYTES      0x4000U
#define ZZAPP_AS_L2_BYTES      0x0400U
/* Bookkeeping bytes per L2 table (owner, list link, virtual section). */
#define ZZAPP_AS_L2_META_BYTES 6U
/* Tables every admitted address space is guaranteed: image sections that mix
 * permissions (about 8), the first alias section, and slack. */
#define ZZAPP_AS_L2_RESERVE    16U
/* The kernel template takes its tables out of the same pool. */
#define ZZAPP_AS_L2_TEMPLATE   16U
/* Hardware ASIDs are 8 bits. 0 is the kernel template; 1..255 are handed out. */
#define ZZAPP_AS_ASID_MAX      255U
/* Alias extents one address space can hold at once. */
#define ZZAPP_AS_ALIAS_EXTENTS 96U

/* Bytes zzapp_as_pool_bytes() returns, as a constant for static pools. */
#define ZZAPP_AS_POOL_BYTES(apps, l2_total) \
	(ZZAPP_AS_L1_BYTES * (1U + (apps)) + \
	 (l2_total) * (ZZAPP_AS_L2_BYTES + ZZAPP_AS_L2_META_BYTES))

#define ZZAPP_AS_IMAGE_RX 1U
#define ZZAPP_AS_IMAGE_RO 2U
#define ZZAPP_AS_IMAGE_RW 3U
/* A page shared by a code segment and a data segment. */
#define ZZAPP_AS_IMAGE_RWX 4U

#define ZZAPP_AS_ALIAS_RO 1U
#define ZZAPP_AS_ALIAS_RW 2U
#define ZZAPP_AS_ALIAS_RX 3U

/* One mapped alias range: first 4 KiB page below ZZAPP_AS_ALIAS_BASE and the
 * page count. Extents stay sorted by `first` and never overlap. */
typedef struct {
	uint16_t first;
	uint16_t pages;
} zzapp_as_extent_t;

typedef struct {
	uint32_t *l1;
	/* Process-record index, the key of the L2 list and the ASID owner. */
	uint32_t pid;
	/* Generation of the ASID assignment; valid only with a matching owner. */
	uint32_t asid_gen;
	/* 0 until the first dispatch, and again after the ASID was stolen. */
	uint16_t asid;
	uint8_t active;
	uint8_t alias_count;
	zzapp_as_extent_t alias[ZZAPP_AS_ALIAS_EXTENTS];
} zzapp_as_t;

typedef struct {
	uint32_t apps;           /* address spaces the pool serves */
	uint32_t l2_total;       /* tables in the pool, template included */
	uint32_t l2_free;        /* tables on the free stack */
	uint32_t l2_template;    /* tables the kernel template holds */
	uint32_t l2_reserved;    /* tables promised to admitted spaces */
	uint32_t l2_reserved_unused; /* promised but not yet allocated */
	/* Tables an allocation outside any reserve can still take. */
	uint32_t l2_shared_free;
	uint32_t tables_refused; /* allocations refused for lack of tables */
	uint32_t asid_assigned;  /* ASIDs currently owned */
	uint32_t asid_steals;    /* ASIDs taken from a live owner */
} zzapp_as_stats_t;

/* ---- Table pool -------------------------------------------------------- */

/* Bytes the pool needs: kernel template L1, `apps` L1 tables, `l2_total` L2
 * tables, and the L2 bookkeeping. Layout in this order, base 16 KiB
 * aligned. */
uint32_t zzapp_as_pool_bytes(uint32_t apps, uint32_t l2_total);
/* Hand the address-space layer its table memory, before zzapp_as_init().
 * 0, or -1 for a misaligned base, a short region, apps outside
 * 1..ZZAPP_AS_MAX_APPS, or an l2_total the template and one reserve cannot
 * fit in. Tables stay valid until the next call; core 1 never frees them. */
int zzapp_as_pool_init(void *base, uint32_t bytes, uint32_t apps,
		       uint32_t l2_total);
/* The L1 table of record `pid`, or NULL outside the pool. */
uint32_t *zzapp_as_pool_l1(uint32_t pid);
void zzapp_as_stats(zzapp_as_stats_t *out);
/* Reserve and release ZZAPP_AS_L2_RESERVE tables for record `pid` at
 * admission. reserve: 0, or ZZAPP_AS_ETABLES when reserved + 16 would exceed
 * the pool less the template (counters untouched), or -1 for a bad pid or a
 * pid that already holds one. release: 0 after freeing any tables the pid
 * still owns and then the reserve (idempotent); -1 for a bad pid or while
 * the pid still has a live address space (destroy it first). */
int zzapp_as_reserve_l2(uint32_t pid);
int zzapp_as_release_l2(uint32_t pid);
/* A table allocation failed for this record outside any caller that can
 * report it (the BSP-change hook). Set by the hook, cleared by destroy. */
int zzapp_as_table_faulted(uint32_t pid);
/* Called, with no lock held, for every pid the BSP-change hook could not
 * give a table. The host stops that record with a fault. */
void zzapp_as_set_table_fault_hook(void (*hook)(uint32_t pid));

/* ---- Address spaces ---------------------------------------------------- */

/* Sections covering executing firmware. Set before init, while the template
 * is not walked. Init splits them to L2 so a later protection change only
 * edits page entries. */
void zzapp_as_set_firmware_text(uint32_t start, uint32_t end);
/* Build the privileged kernel template from the BSP's identity table. The
 * table pool must be set first. */
void zzapp_as_init(const uint32_t *bsp_l1);
/* Copy the template into the L1 table of record `pid`. No ASID is taken
 * until the first dispatch. -1 for a bad pid, a live pid or no pool. */
int zzapp_as_create(zzapp_as_t *as, uint32_t pid);
/* Free the owned L2 tables, zero the L1, retire and free the ASID. The L2
 * reserve is kept: zzapp_as_release_l2() returns it. */
void zzapp_as_destroy(zzapp_as_t *as);
/* Same for the space of record `pid` when its zzapp_as_t was lost (a restarted
 * core 1 cleared it): frees the tables and L1 by index and the ASID by owner.
 * No-op for a pid without a live space. */
void zzapp_as_destroy_pid(uint32_t pid);
/* Core 1 is halted. Free as_lock if that core died holding it. */
void zzapp_as_reset_lock(void);
/* Core 1 is halted. Rebuild every table-pool counter, free stack and list
 * from the ownership marks, so a halt in the middle of an allocation cannot
 * leak or double-own a table. */
void zzapp_as_reconcile(void);
/* Map 1 MiB app frames at a private virtual address. */
int zzapp_as_map_frames(zzapp_as_t *as, uint32_t virtual_base,
			const uint32_t *frames, uint32_t frame_count);
/* Remove 1 MiB private mappings. */
int zzapp_as_unmap_frames(zzapp_as_t *as, uint32_t virtual_base,
                          uint32_t frame_count);
/* Map [phys, phys + bytes) into this app's private alias range. phys and
 * bytes must be page aligned. alignment must be a power-of-two page
 * alignment. RO and RW mappings are XN; RX is executable. */
int zzapp_as_alias_map_flags_aligned(zzapp_as_t *as, uint32_t phys,
				     uint32_t bytes, uint32_t flags,
				     uint32_t alignment, uint32_t *va);
/* Map an XN RO or RW alias with the requested virtual alignment. */
int zzapp_as_alias_map_aligned(zzapp_as_t *as, uint32_t phys, uint32_t bytes,
			       int writable, uint32_t alignment, uint32_t *va);
/* Convenience wrapper for a page-aligned RO, RW, or RX alias. */
int zzapp_as_alias_map_flags(zzapp_as_t *as, uint32_t phys, uint32_t bytes,
			     uint32_t flags, uint32_t *va);
/* Convenience wrapper for a page-aligned XN RO or RW alias. */
int zzapp_as_alias_map(zzapp_as_t *as, uint32_t phys, uint32_t bytes,
		       int writable, uint32_t *va);
/* Unmap exactly the extent a map call returned: va and bytes must match. */
int zzapp_as_alias_unmap(zzapp_as_t *as, uint32_t va, uint32_t bytes);
/* Return the physical address for an alias address, or zero when unmapped. */
uint32_t zzapp_as_alias_phys(const zzapp_as_t *as, uint32_t va);
/* Map page-aligned image memory with the requested user permissions. */
int zzapp_as_map_image(zzapp_as_t *as, uint32_t virtual_base,
			uint32_t physical_base, uint32_t bytes, uint32_t permissions);

/* ---- ASIDs ------------------------------------------------------------- */

/*
 * ASIDs are assigned lazily at the first dispatch and recycled. Core 1 only,
 * local IRQs masked inside, no lock. Whenever an ASID stops belonging to its
 * owner (release, steal) it is flushed from every TLB by ASID, so a free ASID
 * never has live translations and assigning one needs no flush.
 *
 * acquire: the ASID of `as`, assigning one when it holds none or its
 * assignment is stale. Out of free ASIDs it steals the least recently
 * dispatched one whose owner is not the running record. 1..255, or -1 for an
 * inactive space (or when every ASID is the running record's, which cannot
 * happen with a single running record).
 * release: flush and free the ASID of `as`; no-op without a valid one.
 */
int zzapp_as_acquire_asid(zzapp_as_t *as);
void zzapp_as_release_asid(zzapp_as_t *as);
/* Whether `as` still owns the ASID it holds. */
int zzapp_as_asid_valid(const zzapp_as_t *as);
/* Core 1 is halted: every ASID becomes free and the whole TLB is flushed. */
void zzapp_as_reset_asids(void);

/* Install this address space on core 1, assigning an ASID first if needed. */
void zzapp_as_switch(zzapp_as_t *as);
/* Restore core 1's privileged kernel template. */
void zzapp_as_switch_kernel(void);

const uint32_t *zzapp_as_template(void);
const uint32_t *zzapp_as_l1(const zzapp_as_t *as);
const uint32_t *zzapp_as_l2_for(const zzapp_as_t *as, uint32_t address);
/* mmu_page.c calls this after changing a BSP mapping. */
void zzapp_as_mmu_page_changed(uint32_t section, uint32_t descriptor,
				const uint32_t *pages);

#ifdef MMU_PAGE_HOST_TEST
extern unsigned zzapp_as_test_kernel_switches;
extern unsigned zzapp_as_test_asid_invalidations;
/* The ASID of the latest per-ASID invalidation, and the number of whole-TLB
 * invalidations (recovery only). */
extern uint32_t zzapp_as_test_last_invalidated_asid;
extern unsigned zzapp_as_test_tlb_all_invalidations;
void zzapp_as_test_orphan_lock(void);
int zzapp_as_test_lock_held(void);
/* Leave the pool's counters, free stack and lists in the state a core-1 halt
 * in the middle of an allocation could, for reconcile tests. */
void zzapp_as_test_scramble_pool(void);
/* The ASID dispatched last on this host core, 0 for the kernel. */
uint32_t zzapp_as_test_current_asid(void);
#endif

#endif
