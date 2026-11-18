/*
 * Process records, handles, and the boot-time record arena.
 *
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <string.h>

#include "zzapp_rec.h"

#define REC_NONE         0xffffU
/* Set in a state byte between the generation bump and the FREE store of
 * zzapp_rec_free(), so a replay knows the bump already happened. */
#define REC_STATE_BUMPED 0x80U
#define REC_STATE_MASK   0x7fU
#define REC_SERIAL_MASK  0x00ffffffU

#ifdef ZZAPP_HOST_TEST
unsigned zzapp_rec_test_halt_step;
/* Stop after `step` of an ordered operation, as a halted core would. */
#define REC_HALT_AFTER(step) \
	do { if (zzapp_rec_test_halt_step == (step)) return; } while (0)
#define REC_HALT_AFTER_INT(step, ret) \
	do { if (zzapp_rec_test_halt_step == (step)) return (ret); } while (0)
#else
#define REC_HALT_AFTER(step) do { } while (0)
#define REC_HALT_AFTER_INT(step, ret) do { } while (0)
#endif

/* The handle table. A FREE index waits in a FIFO queue linked through next[];
 * next[last] is always REC_NONE. */
static zzapp_rec_table_snapshot_t tab;
static zzapp_rec_layout_t layout;
static uint32_t arena_card;
static uint8_t *area_tables;
static uint8_t *area_stack;
static uint8_t *area_record;
static int ready;
static uint32_t refused_memory;
static uint32_t refused_tables;

#ifdef ZZAPP_HOST_TEST
void zzapp_rec_test_poke_next(uint32_t index, uint32_t next)
{
	if (index < ZZAPP_REC_MAX_APPS)
		tab.next[index] = (uint16_t)next;
}
#endif

static uint32_t round_up(uint32_t value, uint32_t step)
{
	return (value + step - 1U) / step * step;
}

static uint32_t clamp_apps(uint32_t apps)
{
	if (apps < ZZAPP_REC_MIN_APPS)
		return ZZAPP_REC_MIN_APPS;
	return apps > ZZAPP_REC_MAX_APPS ? ZZAPP_REC_MAX_APPS : apps;
}

uint32_t zzapp_rec_apps_for(uint32_t permanent_bytes,
			    uint32_t system_reserve_bytes)
{
	uint32_t usable = permanent_bytes > system_reserve_bytes ?
		permanent_bytes - system_reserve_bytes : 0U;

	return clamp_apps(usable / ZZAPP_REC_APP_BASIS_BYTES);
}

void zzapp_rec_layout_for(uint32_t apps, uint32_t host_heap_bytes,
			  zzapp_rec_layout_t *out)
{
	uint32_t heap_mib;

	if (!out)
		return;
	apps = clamp_apps(apps);
	heap_mib = (host_heap_bytes >> 20) + ((host_heap_bytes & 0xfffffU) != 0U);
	out->apps = apps;
	out->l2_template = ZZAPP_AS_L2_TEMPLATE;
	out->l2_shared = round_up(2U * heap_mib, ZZAPP_REC_SHARED_TABLE_STEP);
	out->l2_total = out->l2_template + apps * ZZAPP_AS_L2_RESERVE +
			out->l2_shared;
	out->as_bytes = zzapp_as_pool_bytes(apps, out->l2_total);
	out->tables_off = round_up(out->as_bytes, ZZAPP_REC_TABLES_BYTES / 2U);
	out->stack_off = out->tables_off + apps * ZZAPP_REC_TABLES_BYTES;
	out->record_off = out->stack_off + apps * ZZAPP_REC_STACK_BYTES;
	out->bytes = round_up(out->record_off + apps * ZZAPP_REC_RECORD_BYTES,
			      ZZAPP_REC_ARENA_ALIGN);
}

static zzapp_rec_t *record_at(uint32_t pid)
{
	return (zzapp_rec_t *)(area_record + (size_t)pid * ZZAPP_REC_RECORD_BYTES);
}

/* The header fields that belong to the index, not to a life of it. */
static void hdr_constants(uint32_t index)
{
	zzapp_rec_hdr_t *hdr = &record_at(index)->hdr;

	hdr->pid = index;
	hdr->kstack_base = (uint32_t)(uintptr_t)(area_stack +
		(size_t)index * ZZAPP_REC_STACK_BYTES);
	hdr->kstack_top = hdr->kstack_base + ZZAPP_REC_STACK_BYTES;
	hdr->tables = (uint32_t)(uintptr_t)(area_tables +
		(size_t)index * ZZAPP_REC_TABLES_BYTES);
	hdr->l1 = (uint32_t)(uintptr_t)zzapp_as_pool_l1(index);
	hdr->window = ZZAPP_REC_NO_WINDOW;
}

int zzapp_rec_arena_init(void *base, const zzapp_rec_layout_t *lay)
{
	uint8_t *p = base;
	uint32_t i;

	ready = 0;
	if (!base || !lay || lay->apps < ZZAPP_REC_MIN_APPS ||
	    lay->apps > ZZAPP_REC_MAX_APPS ||
	    ((uintptr_t)base & (ZZAPP_REC_L1_BYTES - 1U)) != 0U ||
	    lay->tables_off < lay->as_bytes ||
	    lay->stack_off < lay->tables_off + lay->apps * ZZAPP_REC_TABLES_BYTES ||
	    lay->record_off < lay->stack_off + lay->apps * ZZAPP_REC_STACK_BYTES ||
	    lay->bytes < lay->record_off + lay->apps * ZZAPP_REC_RECORD_BYTES ||
	    zzapp_as_pool_init(p, lay->as_bytes, lay->apps, lay->l2_total) != 0)
		return -1;
	layout = *lay;
	arena_card = (uint32_t)(uintptr_t)base;
	area_tables = p + lay->tables_off;
	area_stack = p + lay->stack_off;
	area_record = p + lay->record_off;
	memset(&tab, 0, sizeof(tab));
	for (i = 0U; i < ZZAPP_REC_MAX_APPS; i++) {
		tab.gen[i] = 1U;
		tab.next[i] = REC_NONE;
	}
	for (i = 0U; i + 1U < lay->apps; i++)
		tab.next[i] = (uint16_t)(i + 1U);
	tab.head = 0U;
	tab.tail = lay->apps - 1U;
	tab.free_count = lay->apps;
	refused_memory = 0U;
	refused_tables = 0U;
	/* The arena memory is not zeroed by the card pool. A zero record has a
	 * STOPPED thread, which is what a scheduler that walks every record
	 * before its admission must see. */
	memset(area_record, 0, (size_t)lay->apps * ZZAPP_REC_RECORD_BYTES);
	for (i = 0U; i < lay->apps; i++)
		hdr_constants(i);
	ready = 1;
	return 0;
}

int zzapp_rec_arena_carve(card_pool_t *pool, uint32_t permanent_bytes,
			  uint32_t system_reserve_bytes,
			  uint32_t host_heap_bytes)
{
	const uint32_t owner = CARD_POOL_OWNER(CARD_POOL_CLASS_APP_ARENA, 0U);
	zzapp_rec_layout_t lay;
	uint32_t apps = zzapp_rec_apps_for(permanent_bytes, system_reserve_bytes);
	uint32_t addr;

	for (;;) {
		zzapp_rec_layout_for(apps, host_heap_bytes, &lay);
		addr = card_pool_alloc_aligned(pool, lay.bytes,
					       ZZAPP_REC_ARENA_ALIGN, owner);
		if (addr)
			break;
		if (apps <= ZZAPP_REC_MIN_APPS)
			return -1;
		apps = clamp_apps(apps / 2U);
	}
	if (zzapp_rec_arena_init((void *)(uintptr_t)addr, &lay) != 0) {
		card_pool_free(pool, addr, owner);
		return -1;
	}
	return 0;
}

uint32_t zzapp_rec_arena_base(void)
{
	return ready ? arena_card : 0U;
}

const zzapp_rec_layout_t *zzapp_rec_layout(void)
{
	return ready ? &layout : NULL;
}

uint32_t zzapp_rec_capacity(void)
{
	return ready ? layout.apps : 0U;
}

zzapp_rec_t *zzapp_rec_by_pid(uint32_t pid)
{
	return ready && pid < layout.apps ? record_at(pid) : NULL;
}

zzapp_rec_t *zzapp_rec_by_handle(uint32_t handle)
{
	uint32_t index = aar_handle_index(handle);
	uint32_t generation = aar_handle_generation(handle);
	uint8_t state;

	if (!ready || generation == 0U || index >= layout.apps)
		return NULL;
	/* Generation, state, generation again: a free and re-admit that raced
	 * with this read changes the generation and fails the second compare. */
	if (__atomic_load_n(&tab.gen[index], __ATOMIC_ACQUIRE) != generation)
		return NULL;
	state = __atomic_load_n(&tab.state[index], __ATOMIC_ACQUIRE);
	if ((state & REC_STATE_MASK) == ZZAPP_REC_FREE)
		return NULL;
	if (__atomic_load_n(&tab.gen[index], __ATOMIC_ACQUIRE) != generation)
		return NULL;
	return record_at(index);
}

uint32_t zzapp_rec_handle(uint32_t pid)
{
	if (!ready || pid >= layout.apps ||
	    (tab.state[pid] & REC_STATE_MASK) == ZZAPP_REC_FREE)
		return 0U;
	return record_at(pid)->hdr.handle;
}

zzapp_rec_state_t zzapp_rec_state(uint32_t pid)
{
	if (!ready || pid >= layout.apps)
		return ZZAPP_REC_FREE;
	return (zzapp_rec_state_t)(__atomic_load_n(&tab.state[pid],
						    __ATOMIC_ACQUIRE) &
				   REC_STATE_MASK);
}

int zzapp_rec_set_state(zzapp_rec_t *rec, zzapp_rec_state_t state)
{
	uint32_t pid;
	uint8_t old;

	if (!ready || !rec)
		return -1;
	pid = rec->hdr.pid;
	if (pid >= layout.apps || record_at(pid) != rec)
		return -1;
	old = tab.state[pid];
	if ((old & REC_STATE_MASK) == ZZAPP_REC_FREE ||
	    state == ZZAPP_REC_FREE || state > ZZAPP_REC_RECLAIMED ||
	    state < (old & REC_STATE_MASK))
		return -1;
	__atomic_store_n(&tab.state[pid], (uint8_t)(state | (old & REC_STATE_BUMPED)),
			 __ATOMIC_RELEASE);
	return 0;
}

uint32_t zzapp_rec_stack_base(uint32_t pid)
{
	return ready && pid < layout.apps ? (uint32_t)(uintptr_t)(area_stack +
		(size_t)pid * ZZAPP_REC_STACK_BYTES) : 0U;
}

uint32_t zzapp_rec_stack_top(uint32_t pid)
{
	uint32_t base = zzapp_rec_stack_base(pid);

	return base ? base + ZZAPP_REC_STACK_BYTES : 0U;
}

void *zzapp_rec_tables(uint32_t pid)
{
	return ready && pid < layout.apps ?
		area_tables + (size_t)pid * ZZAPP_REC_TABLES_BYTES : NULL;
}

uint32_t zzapp_rec_admit(uint32_t window, uint32_t *handle, zzapp_rec_t **rec)
{
	zzapp_rec_hdr_t *hdr;
	zzapp_rec_t *r;
	uint32_t index;
	uint32_t serial;

	if (!ready || tab.head == REC_NONE) {
		refused_memory++;
		return AAR_REASON_MEMORY;
	}
	index = tab.head;
	/* A FREE index holds no reserve and no tables. Healing a leftover of a
	 * halted retire here keeps the pool consistent; a space that is still
	 * live (a real leak) makes the reserve fail below. */
	(void)zzapp_as_release_l2(index);
	if (zzapp_as_reserve_l2(index) != 0) {
		refused_tables++;
		return AAR_REASON_TABLES;
	}
	/* Pop the oldest free index. next[index] stays as it was, which lets
	 * zzapp_rec_unadmit() restore the queue bit for bit. */
	tab.head = tab.next[index];
	if (tab.head == REC_NONE)
		tab.tail = REC_NONE;
	tab.free_count--;
	serial = tab.owner_serial;
	tab.owner_serial = (serial + 1U) & REC_SERIAL_MASK;
	if (tab.owner_serial == 0U)
		tab.owner_serial = 1U;
	r = record_at(index);
	memset(r, 0, sizeof(*r));
	hdr_constants(index);
	hdr = &r->hdr;
	hdr->handle = aar_handle_make(index, tab.gen[index]);
	hdr->owner_serial = tab.owner_serial;
	hdr->serial_prev = serial;
	hdr->window = (uint16_t)window;
	hdr->retire_stage = ZZAPP_RETIRE_NONE;
	__atomic_store_n(&tab.state[index], (uint8_t)ZZAPP_REC_CLAIMED,
			 __ATOMIC_RELEASE);
	if (handle)
		*handle = hdr->handle;
	if (rec)
		*rec = r;
	return 0U;
}

int zzapp_rec_unadmit(uint32_t handle)
{
	zzapp_rec_t *r = zzapp_rec_by_handle(handle);
	uint32_t index;

	if (!r || (tab.state[r->hdr.pid] & REC_STATE_MASK) != ZZAPP_REC_CLAIMED)
		return -1;
	index = r->hdr.pid;
	if (zzapp_as_release_l2(index) != 0)
		return -1;
	if (tab.owner_serial == r->hdr.owner_serial)
		tab.owner_serial = r->hdr.serial_prev;
	r->hdr.handle = 0U;
	r->hdr.owner_serial = 0U;
	r->hdr.serial_prev = 0U;
	r->hdr.window = ZZAPP_REC_NO_WINDOW;
	r->hdr.retire_stage = ZZAPP_RETIRE_NONE;
	__atomic_store_n(&tab.state[index], (uint8_t)ZZAPP_REC_FREE,
			 __ATOMIC_RELEASE);
	/* Back to the head: the queue is as it was before the pop. */
	tab.next[index] = (uint16_t)tab.head;
	if (tab.head == REC_NONE)
		tab.tail = index;
	tab.head = index;
	tab.free_count++;
	return 0;
}

void zzapp_rec_set_stage(zzapp_rec_t *rec, zzapp_retire_stage_t stage)
{
	if (rec && stage > rec->hdr.retire_stage)
		rec->hdr.retire_stage = (uint8_t)stage;
}

void zzapp_rec_retire_tables(zzapp_rec_t *rec)
{
	uint32_t pid;

	if (!ready || !rec)
		return;
	pid = rec->hdr.pid;
	if (rec->proc.as.active)
		zzapp_as_destroy(&rec->proc.as);
	/* A space whose struct was lost (a restarted core 1 cleared it) is still
	 * live in the pool; free it by index. */
	zzapp_as_destroy_pid(pid);
	REC_HALT_AFTER(1U);
	(void)zzapp_as_release_l2(pid);
	REC_HALT_AFTER(2U);
	zzapp_rec_set_stage(rec, ZZAPP_RETIRE_TABLES);
}

static uint16_t prev_generation(uint32_t generation)
{
	return (uint16_t)(generation <= 1U ? 0xffffU : generation - 1U);
}

int zzapp_rec_free(uint32_t handle)
{
	uint32_t index = aar_handle_index(handle);
	uint32_t generation = aar_handle_generation(handle);
	uint8_t state;
	int bumped;

	if (!ready || generation == 0U || index >= layout.apps)
		return -1;
	state = tab.state[index];
	bumped = (state & REC_STATE_BUMPED) != 0U;
	if ((state & REC_STATE_MASK) == ZZAPP_REC_FREE) {
		/* Replay of a free that completed: the generation moved on by
		 * exactly this free. */
		return generation == prev_generation(tab.gen[index]) ? 0 : -1;
	}
	if (generation != (bumped ? prev_generation(tab.gen[index]) :
			   tab.gen[index]))
		return -1;
	if (!bumped) {
		/* 1: no stale handle can match from here on. */
		__atomic_store_n(&tab.gen[index],
				 (uint16_t)aar_handle_next_gen(tab.gen[index]),
				 __ATOMIC_RELEASE);
		__atomic_store_n(&tab.state[index],
				 (uint8_t)(state | REC_STATE_BUMPED),
				 __ATOMIC_RELEASE);
	}
	REC_HALT_AFTER_INT(1U, 0);
	/* 2: publish FREE. */
	record_at(index)->hdr.handle = 0U;
	__atomic_store_n(&tab.state[index], (uint8_t)ZZAPP_REC_FREE,
			 __ATOMIC_RELEASE);
	REC_HALT_AFTER_INT(2U, 0);
	/* 3: queue at the tail. */
	tab.next[index] = REC_NONE;
	if (tab.tail != REC_NONE)
		tab.next[tab.tail] = (uint16_t)index;
	else
		tab.head = index;
	tab.tail = index;
	tab.free_count++;
	return 0;
}

uint32_t zzapp_rec_reconcile_halted(void)
{
	static uint16_t order[ZZAPP_REC_MAX_APPS];
	uint8_t seen[ZZAPP_REC_MAX_APPS];
	uint32_t index;
	uint32_t steps = 0U;
	uint32_t count = 0U;
	uint32_t busy = 0U;
	uint32_t i;

	if (!ready)
		return 0U;
	zzapp_as_reconcile();
	memset(seen, 0, sizeof(seen));
	/* Keep the order of the entries that are still properly linked: follow
	 * the old chain from the head while it names FREE indices, once each. */
	for (index = tab.head; index < layout.apps && steps++ < layout.apps &&
	     !seen[index] &&
	     (tab.state[index] & REC_STATE_MASK) == ZZAPP_REC_FREE;
	     index = tab.next[index]) {
		seen[index] = 1U;
		order[count++] = (uint16_t)index;
	}
	/* A FREE index the chain lost (halt between FREE and the queue). */
	for (i = 0U; i < layout.apps; i++) {
		if ((tab.state[i] & REC_STATE_MASK) != ZZAPP_REC_FREE)
			busy++;
		else if (!seen[i])
			order[count++] = (uint16_t)i;
	}
	for (i = 0U; i < count; i++)
		tab.next[order[i]] = i + 1U < count ? order[i + 1U] : REC_NONE;
	tab.head = count ? order[0] : REC_NONE;
	tab.tail = count ? order[count - 1U] : REC_NONE;
	tab.free_count = count;
	return busy;
}

void zzapp_rec_stats(zzapp_rec_stats_t *out)
{
	uint32_t i;

	if (!out)
		return;
	memset(out, 0, sizeof(*out));
	if (!ready)
		return;
	out->capacity = layout.apps;
	for (i = 0U; i < layout.apps; i++) {
		switch (tab.state[i] & REC_STATE_MASK) {
		case ZZAPP_REC_FREE:
			out->free++;
			break;
		case ZZAPP_REC_CLAIMED:
			out->claimed++;
			break;
		case ZZAPP_REC_LIVE:
			out->live++;
			break;
		case ZZAPP_REC_RETIRING:
			out->retiring++;
			break;
		default:
			out->reclaimed++;
			break;
		}
	}
	out->refused_memory = refused_memory;
	out->refused_tables = refused_tables;
	out->owner_serial = tab.owner_serial;
}

void zzapp_rec_table_snapshot(zzapp_rec_table_snapshot_t *out)
{
	if (out)
		*out = tab;
}
