/*
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 *
 * Rings, file seqlock, input, log, display bind, and the app audio producer.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "zzapp_services.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "aar/app.h"
#include "zzapp_proto.h"
#include "zzapp_runtime.h"
void zzapp_host_yield_wait(void);
void __attribute__((weak)) zzapp_status_observed(void) {}
void __attribute__((weak)) zzapp_host_latch_stop(uint32_t slot)
{
	(void)slot;
}
/* Largest host-visible limit a launcher may request. The host runtime
 * defines it; a build that links only the services gets no ceiling. */
uint32_t __attribute__((weak)) zzapp_host_visible_capacity(void)
{
	return UINT32_MAX;
}
/* The groups the host runtime serves. A build that links only the services
 * has no table, so every group a launcher can use counts as present. */
uint32_t __attribute__((weak)) zzapp_runtime_groups(void)
{
	return AAR_GROUP_DISPLAY | AAR_GROUP_INPUT | AAR_GROUP_AUDIO |
	       AAR_GROUP_GL | AAR_GROUP_SERVICES;
}
extern int zzapp_host_take_low_memory_event(uint32_t slot)
	__attribute__((weak));
extern int zzapp_host_take_stop_event(uint32_t slot)
	__attribute__((weak));
extern int zzapp_host_note_stop_event_delivered(uint32_t slot)
	__attribute__((weak));
extern void zzapp_host_reset_low_memory(uint32_t slot)
	__attribute__((weak));
extern void zzapp_host_claimed(uint32_t slot)
	__attribute__((weak));
extern void zzapp_host_quarantine_released(uint32_t slot)
	__attribute__((weak));
#include "aar/app.h"
#ifdef ZZAPP_HOST_TEST
#include <stdlib.h>
/* Host recorder for the present-path flush: production code decides what
 * to clean; suites assert the presented card range and that the size is
 * the surface, never more. */
struct zzapp_present_flush_record {
	unsigned long card;
	unsigned long bytes;
};
struct zzapp_present_flush_record zzapp_test_present_flushes[32];
unsigned zzapp_test_present_flush_count;
__attribute__((weak)) int scheduler_core1_available(void) { return 1; }
#else
#undef SDK_STATUS_OK
#undef SDK_STATUS_QUEUED
#undef SDK_STATUS_BUSY
#undef SDK_STATUS_UNSUPPORTED
#undef SDK_STATUS_BAD_REQUEST
#undef SDK_STATUS_BAD_HANDLE
#undef SDK_STATUS_NO_MEMORY
#undef SDK_STATUS_IO_ERROR
#undef SDK_STATUS_NOT_FOUND
#include <stdio.h>
#include "audio_fabric.h"
#include "audio_fabric_internal.h"
#include "memorymap.h"
#include "l2_maint.h"
#include "scheduler.h"
#include "sdk_aperture_layout.h"
#include "sdk_smp_lock.h"
#include "surface_allocator.h"
#include "xil_cache.h"
#ifndef ZZAPP_HOST_TEST
typedef char zzapp_window_size_matches_memorymap[
	(ZZAPP_WINDOW_BYTES == ZZAPP_WINDOW_Z3_SIZE) ? 1 : -1];
#endif
#endif

#ifndef ZZAPP_FILE_TIMEOUT_US
#define ZZAPP_FILE_TIMEOUT_US 30000000ull
#endif
/*
 * Loading advances in 64 KiB chunks. A launcher heartbeat, load begin, or
 * completed chunk renews its file-I/O lease before an abandoned arena is
 * quarantined.
 */
#define ZZAPP_LOAD_LEASE_US ZZAPP_FILE_TIMEOUT_US
#define ZZAPP_CLAIM_LEASE_US ZZAPP_LOAD_LEASE_US
#define ZZAPP_LOG_RING_HZ     32u
#define ZZAPP_LOG_UART_HZ     8u
#define ZZAPP_AUDIO_FRAMES    8192u
#define ZZAPP_AUDIO_RATE      22050u
#define ZZAPP_P96_LO          0x00200000u
#define ZZAPP_P96_HI          0x033F0000u
#define ZZAPP_HEAP_LO         0x033F0000u
#define ZZAPP_HEAP_HI         0x08200000u
#define ZZAPP_UNCACHED_LO     0x08100000u
#define ZZAPP_UNCACHED_HI     0x08200000u

#ifndef AUDIO_FABRIC_SLOT_MAILBOX
#define AUDIO_FABRIC_SLOT_MAILBOX  1u
#define AUDIO_FABRIC_SLOT_RESERVED 2u
#endif

enum {
	DEFER_IDLE = 0,
	DEFER_FILLING = 1,
	DEFER_POSTED = 2,
	DEFER_RUNNING = 3,
	DEFER_DONE = 4,
	AUDIO_REQUEST_NONE = 0,
	AUDIO_REQUEST_OPEN = 1,
	AUDIO_REQUEST_CLOSE = 2,
	AUDIO_REQUEST_CLOSE_QUIESCED = 3
};

enum zzapp_wait_kind {
	ZZAPP_WAIT_NONE,
	ZZAPP_WAIT_FILE,
	ZZAPP_WAIT_CONSOLE_WRITE,
	ZZAPP_WAIT_CONSOLE_READ,
	ZZAPP_WAIT_DISPLAY_OPEN,
	ZZAPP_WAIT_FLIP,
	ZZAPP_WAIT_AUDIO,
	ZZAPP_WAIT_EVENT
};

struct zzapp_service_slot {
	uint8_t *window;
	uint32_t window_bytes;
	uint32_t generation;
	uint32_t state;
	uint32_t memory_limit;
	uint32_t host_visible_limit;
	uint32_t terminal_reason;
	uint64_t claim_started;
	uint64_t load_progress;
	uint32_t load_step;
	uint32_t launcher_heartbeat;
	int quarantined;
	int quarantine_reclaimed;
	int quarantine_release_requested;
	uint8_t name[ZZAPP_LIST_NAME_BYTES];
	int window_uncached;
	int legacy_claim;
};

static void release_quarantine_locked(struct zzapp_service_slot *entry);

struct zzapp_service_runtime {
	uint32_t file_seq;
	int file_open[ZZAPP_FD_COUNT];
	int list_open[ZZAPP_FD_COUNT];
	volatile int display_bound;
	int display_back_owned;
	uint32_t display_id;
	uint32_t display_width;
	uint32_t display_height;
	uint32_t display_pitch;
	uint32_t display_format;
	uint32_t display_layout;
	uint32_t display_bytes;
	uint32_t display_front;
	uint32_t display_back;
	uint32_t display_spare;
	uint32_t display_present;
	uint32_t display_alias_phys[3];
	uint32_t display_alias[3];
	uint32_t display_flip_gen;
	uint32_t display_ack_seen;
	uint32_t display_queued;
	volatile int display_mailbox_active;
	uint32_t display_mailbox_shown;
	uint32_t display_mailbox_pending;
	uint32_t display_mailbox_buffers[3];
	uint32_t display_mailbox_bytes;
	uint32_t display_grant_base[3];
	uint32_t display_grant_bytes[3];
	int display_mailbox_latch_pending;
#ifndef ZZAPP_HOST_TEST
	sdk_smp_lock_t display_mailbox_lock;
#else
	volatile uint32_t display_mailbox_lock;
#endif
	uint32_t log_ring_count;
	uint64_t log_ring_window;
#ifndef ZZAPP_HOST_TEST
	uint32_t log_uart_count;
	uint64_t log_uart_window;
#endif
	volatile int audio_open;
	uint32_t audio_slot;
	volatile uint32_t *audio_read_frame;
	volatile uint32_t *audio_write_frame;
	int16_t *audio_samples;
	uint64_t audio_staged;
	uint32_t audio_ahi_staged_frame;
	volatile uint32_t audio_first_write_seen;
	void *audio_block;
	uint32_t audio_grant_base;
	uint32_t audio_grant_bytes;
	uint32_t audio_alias;
	volatile uint32_t audio_request;
	int audio_result;
	int audio_teardown;
	uint32_t audio_stream_epoch;
	int audio_ahi_route;
	uint32_t audio_host_tail_seen;
#if ZZAPP_PERF_COUNTERS
	uint8_t audio_low_water_latched;
#endif
	enum zzapp_wait_kind wait_kind;
	uint32_t wait_value;
	uint64_t wait_deadline;
#ifdef ZZAPP_HOST_TEST
	uint32_t audio_detach_count;
#endif
};

struct zzapp_deferred {
	volatile uint32_t state;
	uint16_t opcode;
	uint16_t status;
	uint16_t req_len;
	uint16_t reply_len;
	uint32_t request_id;
	uint32_t cookie;
	uint32_t generation;
	volatile uint32_t cancelled;
	uint8_t req[ZZAPP_MAILBOX_PAYLOAD_BYTES];
	uint8_t reply[ZZAPP_MAILBOX_PAYLOAD_BYTES];
	volatile uint32_t start_pending;
	uint32_t start_argc;
};

static struct zzapp_service_slot slots[ZZAPP_SLOT_COUNT];
static struct zzapp_service_runtime runtime[ZZAPP_SLOT_COUNT];
static struct zzapp_deferred deferred[ZZAPP_SLOT_COUNT];
static uint32_t selected_slot[2];
static uint32_t deferred_harvest_slot = ZZAPP_SLOT_COUNT;
static uint32_t deferred_poll_next_slot;
static int window_mapping_available;
static uint32_t load_diag_seen;
static int slot_busy_test[4];
static int display_mailbox_slot = -1;
#ifndef ZZAPP_HOST_TEST
static sdk_smp_lock_t g_slot_state_lock = SDK_SMP_LOCK_INIT;
#else
static volatile uint32_t g_slot_state_test_lock;
#endif

static void (*yield_hook)(void);
static uint64_t (*time_hook)(void);
static void slot_state_lock_acquire(void)
{
#ifdef ZZAPP_HOST_TEST
	while (__atomic_test_and_set(&g_slot_state_test_lock, __ATOMIC_ACQUIRE))
		;
#else
	sdk_smp_lock_acquire(&g_slot_state_lock);
#endif
}

static void slot_state_lock_release(void)
{
#ifdef ZZAPP_HOST_TEST
	__atomic_clear(&g_slot_state_test_lock, __ATOMIC_RELEASE);
#else
	sdk_smp_lock_release(&g_slot_state_lock);
#endif
}

static unsigned service_context(void);

static uint32_t service_slot(void)
{
	int slot = zzapp_kernel_current_thread();

	if (service_context() == 1U && slot >= 0 &&
	    slot < (int)ZZAPP_AS_SLOT_COUNT)
		return (uint32_t)slot;
	return selected_slot[service_context()];
}

static struct zzapp_service_runtime *runtime_for_slot(uint32_t slot)
{
	return slot < ZZAPP_SLOT_COUNT ? &runtime[slot] : NULL;
}

static struct zzapp_service_runtime *current_runtime(void)
{
	return runtime_for_slot(service_slot());
}
static int wait_status(void)
{
	int status = zzapp_kernel_wait_status(zzapp_runtime_kernel(), service_slot());

	if (status == ZZAPP_KERNEL_WAIT_TIMEOUT)
		return AAR_E_TIMEDOUT;
	if (status == ZZAPP_KERNEL_WAIT_CANCELLED)
		return AAR_E_IO;
	return AAR_OK;
}

static void wait_begin(enum zzapp_wait_kind kind, uint32_t value,
		       uint64_t deadline)
{
	struct zzapp_service_runtime *state = current_runtime();

	state->wait_kind = kind;
	state->wait_value = value;
	state->wait_deadline = deadline;
}

static void wait_end(void)
{
	struct zzapp_service_runtime *state = current_runtime();

	state->wait_kind = ZZAPP_WAIT_NONE;
	state->wait_value = 0U;
	state->wait_deadline = 0U;
}

int zzapp_services_wait_registered(uint32_t slot)
{
	struct zzapp_service_runtime *state = runtime_for_slot(slot);

	return state && state->wait_kind != ZZAPP_WAIT_NONE;
}


#ifndef ZZAPP_SERVICES_STANDALONE_TEST
static zzapp_kernel_t service_fallback_kernel;
static zzapp_proc_t service_fallback_proc[ZZAPP_AS_SLOT_COUNT];

__attribute__((weak)) int zzapp_kernel_current_thread(void)
{
	return -1;
}

__attribute__((weak)) zzapp_kernel_t *zzapp_runtime_kernel(void)
{
	return &service_fallback_kernel;
}

#if ZZAPP_PERF_COUNTERS
__attribute__((weak)) void zzapp_kernel_perf_audio(zzapp_kernel_t *kernel,
						     uint32_t slot,
						     uint32_t low_water,
						     uint32_t starved,
						     uint32_t underruns)
{
	(void)kernel;
	(void)slot;
	(void)low_water;
	(void)starved;
	(void)underruns;
}
#endif

__attribute__((weak)) zzapp_proc_t *zzapp_proc_slot(uint32_t slot)
{
	return slot < ZZAPP_AS_SLOT_COUNT ? &service_fallback_proc[slot] : NULL;
}

__attribute__((weak)) int zzapp_kernel_audio_open(zzapp_kernel_t *kernel,
						    uint32_t slot, int direct)
{
	if (!kernel || slot >= ZZAPP_AS_SLOT_COUNT ||
	    (direct && kernel->direct_audio_count >= 2U))
		return -1;
	if (kernel->resources[slot].direct_audio)
		return -1;
	kernel->resources[slot].direct_audio = direct ? 1U : 2U;
	if (direct)
		kernel->direct_audio_count++;
	return 0;
}

__attribute__((weak)) void zzapp_kernel_audio_close(zzapp_kernel_t *kernel,
						      uint32_t slot)
{
	if (!kernel || slot >= ZZAPP_AS_SLOT_COUNT)
		return;
	if (kernel->resources[slot].direct_audio == 1U &&
	    kernel->direct_audio_count != 0U)
		kernel->direct_audio_count--;
	kernel->resources[slot].direct_audio = 0U;
}
__attribute__((weak)) int zzapp_kernel_wait_status(
	const zzapp_kernel_t *kernel, uint32_t slot)
{
	if (!kernel || slot >= ZZAPP_AS_SLOT_COUNT)
		return ZZAPP_KERNEL_WAIT_CANCELLED;
	if (kernel->threads[slot].wake_reason == ZZAPP_WAKE_TIMEOUT)
		return ZZAPP_KERNEL_WAIT_TIMEOUT;
	if (kernel->threads[slot].wake_reason == ZZAPP_WAKE_CANCELLED)
		return ZZAPP_KERNEL_WAIT_CANCELLED;
	return 0;
}

__attribute__((weak)) int zzapp_kernel_wake(zzapp_kernel_t *kernel,
					      uint32_t slot,
					      zzapp_wake_reason_t reason)
{
	if (!kernel || slot >= ZZAPP_AS_SLOT_COUNT)
		return -1;
	kernel->threads[slot].state = ZZAPP_THREAD_RUNNABLE;
	kernel->threads[slot].wake_reason = reason;
	return 0;
}

__attribute__((weak)) int zzapp_kernel_surface_bind(zzapp_kernel_t *kernel,
						      uint32_t slot,
						      uint32_t generation,
						      uint32_t base,
						      uint32_t bytes)
{
	(void)kernel;
	(void)slot;
	(void)generation;
	return base != 0U && bytes != 0U ? 0 : -1;
}
__attribute__((weak)) void zzapp_kernel_surface_unbind(
	zzapp_kernel_t *kernel, uint32_t slot)
{
	(void)kernel;
	(void)slot;
}
#ifdef ZZAPP_HOST_TEST

__attribute__((weak)) int zzapp_as_alias_map(zzapp_as_t *as, uint32_t phys,
					      uint32_t bytes, int writable,
					      uint32_t *va)
{
	(void)as;
	(void)bytes;
	(void)writable;
	if (!va)
		return -1;
	*va = phys;
	return 0;
}

__attribute__((weak)) int zzapp_as_alias_unmap(zzapp_as_t *as, uint32_t va,
						uint32_t bytes)
{
	(void)as;
	(void)va;
	(void)bytes;
	return 0;
}

__attribute__((weak)) int zzapp_loader_set_generation(uint32_t generation)
{
	return generation != 0U ? AAR_OK : AAR_E_INVAL;
}
#endif



#endif
#ifdef ZZAPP_HOST_TEST
static int defer_test_core = 1;
#else
static uint32_t defer_wrong_core_logged;
#endif

static void barrier(void)
{
#if defined(__arm__) && !defined(ZZAPP_HOST_TEST)
	__asm__ __volatile__("dsb sy" ::: "memory");
#else
	__asm__ __volatile__("" ::: "memory");
#endif
}

static void display_mailbox_lock_acquire_for(
	struct zzapp_service_runtime *state)
{
#ifndef ZZAPP_HOST_TEST
	sdk_smp_lock_acquire(&state->display_mailbox_lock);
#else
	while (__atomic_test_and_set(&state->display_mailbox_lock, __ATOMIC_ACQUIRE))
		;
#endif
}

static void display_mailbox_lock_release_for(
	struct zzapp_service_runtime *state)
{
#ifndef ZZAPP_HOST_TEST
	sdk_smp_lock_release(&state->display_mailbox_lock);
#else
	__atomic_clear(&state->display_mailbox_lock, __ATOMIC_RELEASE);
#endif
}

static void display_mailbox_lock_acquire(void)
{
	display_mailbox_lock_acquire_for(current_runtime());
}

static void display_mailbox_lock_release(void)
{
	display_mailbox_lock_release_for(current_runtime());
}

static int display_mailbox_owns_state(
	const struct zzapp_service_runtime *state, uint32_t address)
{
	uint32_t i;

	for (i = 0U; i < 3U; i++) {
		uint32_t base = state->display_mailbox_buffers[i];

		if (address >= base && address - base < state->display_mailbox_bytes)
			return 1;
	}
	return 0;
}


void zzapp_services_reset_core1_lock(void)
{
	uint32_t slot;

	/* Core 1 can hold any slot's mailbox lock, not only the selected one. */
	for (slot = 0U; slot < ZZAPP_SLOT_COUNT; slot++) {
#ifndef ZZAPP_HOST_TEST
		sdk_smp_lock_reset(&runtime[slot].display_mailbox_lock);
#else
		__atomic_clear(&runtime[slot].display_mailbox_lock, __ATOMIC_RELEASE);
#endif
	}
#ifndef ZZAPP_HOST_TEST
	sdk_smp_lock_reset(&g_slot_state_lock);
#else
	__atomic_clear(&g_slot_state_test_lock, __ATOMIC_RELEASE);
#endif
}

static int display_mailbox_enabled(void)
{
	return __atomic_load_n(&current_runtime()->display_mailbox_active, __ATOMIC_ACQUIRE);
}

static void display_mailbox_disable(void)
{
	display_mailbox_lock_acquire();
	__atomic_store_n(&current_runtime()->display_mailbox_active, 0, __ATOMIC_RELEASE);
	current_runtime()->display_mailbox_pending = 0U;
	current_runtime()->display_mailbox_latch_pending = 0;
	display_mailbox_lock_release();
}

static struct zzapp_deferred *defer_for_slot(uint32_t slot)
{
	return slot < ZZAPP_SLOT_COUNT ? &deferred[slot] : NULL;
}

static struct zzapp_deferred *current_deferred(void)
{
	return defer_for_slot(service_slot());
}

static uint32_t defer_load(void)
{
	return __atomic_load_n(&current_deferred()->state, __ATOMIC_ACQUIRE);
}


static int defer_cancelled(const struct zzapp_deferred *defer)
{
	return __atomic_load_n(&defer->cancelled, __ATOMIC_ACQUIRE) != 0U;
}
/* Callers hold g_slot_state_lock.  A zero return leaves core 1 responsible
 * for the RUNNING record; a negative return preserves a completed load. */
static int stop_current_deferred(void)
{
	struct zzapp_deferred *defer = current_deferred();

	if (defer->state == DEFER_RUNNING) {
		defer->cancelled = 1U;
		return 0;
	}
	if (defer->state == DEFER_DONE)
		return -1;
	if (defer->state != DEFER_IDLE) {
		defer->state = DEFER_IDLE;
		if (deferred_harvest_slot == service_slot())
			deferred_harvest_slot = ZZAPP_SLOT_COUNT;
	}
	return 1;
}

static int cancel_running_current_deferred(void)
{
	struct zzapp_deferred *defer = current_deferred();

	if (defer->state != DEFER_RUNNING)
		return 0;
	defer->cancelled = 1U;
	return 1;
}

static int defer_on_core1(void)
{
#ifdef ZZAPP_HOST_TEST
	return defer_test_core == 1;
#else
	return smp_cpu_id() == 1;
#endif
}
static unsigned service_context(void)
{
#ifdef ZZAPP_HOST_TEST
	return defer_test_core == 0 ? 0U : 1U;
#else
	return smp_cpu_id() == 0 ? 0U : 1U;
#endif
}


static void select_slot(uint32_t slot)
{
	if (slot >= ZZAPP_SLOT_COUNT)
		slot = 0U;
	selected_slot[service_context()] = slot;
}

static uint8_t *current_window(void)
{
	return slots[service_slot()].window;
}

static uint32_t current_window_bytes(void)
{
	return slots[service_slot()].window_bytes;
}

static int __attribute__((unused)) current_window_uncached(void)
{
	return slots[service_slot()].window_uncached;
}

static int select_request_slot(const uint8_t *req, uint16_t req_len,
			       int check_generation)
{
	uint32_t slot = 0U;

	if (req_len == ZZAPP_MAILBOX_PAYLOAD_BYTES) {
		if (!req)
			return SDK_STATUS_BAD_REQUEST;
		slot = zzapp_get_be32(req + ZZAPP_SEL_SLOT);
		if (slot >= ZZAPP_SLOT_COUNT)
			return SDK_STATUS_BAD_HANDLE;
		if (check_generation &&
		    zzapp_get_be32(req + ZZAPP_SEL_GENERATION) !=
		    slots[slot].generation)
			return SDK_STATUS_BAD_HANDLE;
	}
	select_slot(slot);
	return SDK_STATUS_OK;
}

static int have_window(void)
{
	return current_window() != NULL &&
	       current_window_bytes() >= ZZAPP_WINDOW_BYTES;
}

static void win_flush(uint32_t off, uint32_t len)
{
#ifndef ZZAPP_HOST_TEST
	if (!current_window_uncached() && have_window() && len != 0U)
		Xil_DCacheFlushRange((INTPTR)(current_window() + off), len);
#else
	(void)off;
	(void)len;
#endif
}

static void win_inval(uint32_t off, uint32_t len)
{
#ifndef ZZAPP_HOST_TEST
	if (!current_window_uncached() && have_window() && len != 0U)
		Xil_DCacheInvalidateRange((INTPTR)(current_window() + off), len);
#else
	(void)off;
	(void)len;
#endif
}

static void win_put32(uint32_t off, uint32_t value)
{
	zzapp_put_be32(current_window() + off, value);
	win_flush(off, 4U);
}

static uint32_t win_get32(uint32_t off)
{
	volatile const uint32_t *word =
		(volatile const uint32_t *)(current_window() + off);
	uint32_t value;

	win_inval(off, 4U);
	value = __builtin_bswap32(*word);
	barrier();
	return value;
}

static void life_put(uint32_t off, uint32_t value)
{
	if (!have_window())
		return;
	win_put32(ZZAPP_LIFE_OFF + off, value);
}
static void life_put_slot(uint32_t slot, uint32_t off, uint32_t value)
{
	struct zzapp_service_slot *entry;

	if (slot >= ZZAPP_SLOT_COUNT)
		return;
	entry = &slots[slot];
	if (!entry->window || entry->window_bytes < ZZAPP_WINDOW_BYTES)
		return;
	zzapp_put_be32(entry->window + ZZAPP_LIFE_OFF + off, value);
#ifndef ZZAPP_HOST_TEST
	if (!entry->window_uncached)
		Xil_DCacheFlushRange((INTPTR)(entry->window + ZZAPP_LIFE_OFF + off),
				     4U);
#endif
}

void zzapp_services_set_low_memory_crossings(uint32_t slot, uint32_t crossings)
{
	life_put_slot(slot, ZZAPP_LIFE_LOW_MEMORY_CROSSINGS_OFF, crossings);
}


static uint32_t life_get(uint32_t off)
{
	if (!have_window())
		return 0U;
	return win_get32(ZZAPP_LIFE_OFF + off);
}

#ifndef ZZAPP_HOST_TEST
static int raw_life_get(uint32_t off, uint32_t *value)
{
	if (!current_window() ||
	    current_window_bytes() < ZZAPP_LIFE_OFF + off + 4U)
		return 0;
	*value = win_get32(ZZAPP_LIFE_OFF + off);
	return 1;
}

static int load_diag_once(uint16_t opcode)
{
	uint32_t bit;

	if (opcode < ZZAPP_OP_FIRST || opcode - ZZAPP_OP_FIRST >= 32U)
		return 1;
	bit = 1U << (opcode - ZZAPP_OP_FIRST);
	if (load_diag_seen & bit)
		return 0;
	load_diag_seen |= bit;
	return 1;
}

static uint64_t now_us(void);
static uint64_t reject_report_last_us;
static uint32_t reject_report_held;

/* A rejection prints every time, so a slot that refuses all loads leaves a
 * line per refusal. A client that retries in a loop is held to four lines a
 * second; the next line carries the count it held back. */
static int reject_report_allowed(void)
{
	uint64_t now = now_us();

	if (reject_report_last_us != 0U &&
	    now - reject_report_last_us < 250000ULL) {
		reject_report_held++;
		return 0;
	}
	reject_report_last_us = now;
	if (reject_report_held != 0U) {
		printf("[app] %lu rejected ops held back\r\n",
		       (unsigned long)reject_report_held);
		reject_report_held = 0U;
	}
	return 1;
}
#endif

static void report_load_snapshot(const char *event, uint16_t opcode,
					 const char *reason)
{
#ifndef ZZAPP_HOST_TEST
	uint32_t life_state;
	uint32_t life_stop;
	int raw_life;

	if (strcmp(event, "reject") == 0 ? !reject_report_allowed() :
					   !load_diag_once(opcode))
		return;
	raw_life = raw_life_get(ZZAPP_LIFE_STATE_OFF, &life_state) &&
		raw_life_get(ZZAPP_LIFE_STOP_OFF, &life_stop);
	if (raw_life) {
		printf("[app] %s op 0x%04x%s%s%s: have_window=%d window=%p bytes=%lu uncached=%d life_state=0x%08lx life_stop=0x%08lx loader_state=0x%08lx reset_request=0x%08lx core=%d\r\n",
		       event, (unsigned)opcode, reason ? " (" : "",
		       reason ? reason : "", reason ? ")" : "", have_window(),
		       (void *)current_window(),
		       (unsigned long)current_window_bytes(),
		       current_window_uncached(),
		       (unsigned long)life_state, (unsigned long)life_stop,
		       (unsigned long)zzapp_loader_state(),
		       (unsigned long)zzapp_loader_reset_requested(), smp_cpu_id());
	} else {
		printf("[app] %s op 0x%04x%s%s%s: have_window=%d window=%p bytes=%lu uncached=%d life_state=n/a life_stop=n/a loader_state=0x%08lx reset_request=0x%08lx core=%d\r\n",
		       event, (unsigned)opcode, reason ? " (" : "",
		       reason ? reason : "", reason ? ")" : "", have_window(),
		       (void *)current_window(),
		       (unsigned long)current_window_bytes(),
		       current_window_uncached(),
		       (unsigned long)zzapp_loader_state(),
		       (unsigned long)zzapp_loader_reset_requested(), smp_cpu_id());
	}
#else
	(void)event;
	(void)opcode;
	(void)reason;
#endif
}

static void yield_once(void)
{
	if (yield_hook)
		yield_hook();
	else
		zzapp_host_yield();
}

static void yield_wait_once(void)
{
	if (yield_hook)
		yield_hook();
	else
		zzapp_host_yield_wait();
}

static uint64_t now_us(void)
{
	if (time_hook)
		return time_hook();
	return zzapp_host_time_us();
}

static void report_rejected_app_op(uint16_t opcode, const char *reason)
{
#ifndef ZZAPP_HOST_TEST
	if (reject_report_allowed())
		printf("[app] reject op 0x%04x: %s\r\n", (unsigned)opcode, reason);
#else
	(void)opcode;
	(void)reason;
#endif
}

#ifdef ZZAPP_SERVICES_STANDALONE_TEST

static uint32_t service_loader_state;
static zzapp_kernel_t service_test_kernel;
static zzapp_proc_t service_test_proc[ZZAPP_AS_SLOT_COUNT];
static int service_test_current_thread = -1;
static unsigned service_test_wake_count;
static uint32_t service_loader_generation;
static uint32_t service_test_grant_count;
static uint32_t service_test_ungrant_count;
static void (*service_test_loader_begin_hook)(void);
static void (*service_test_loader_set_generation_hook)(void);
static int (*service_test_loader_chunk_hook)(uint32_t file_off,
					      const void *src, uint32_t len);
static int (*service_test_loader_end_hook)(void);
static void (*service_test_deferred_completion_hook)(void);
static void (*service_test_deferred_completing_hook)(void);
static void (*service_test_deferred_reply_hook)(void);
static unsigned service_test_loader_abort_count;

__attribute__((weak)) int zzapp_as_alias_map(zzapp_as_t *as, uint32_t phys,
					      uint32_t bytes, int writable,
					      uint32_t *va)
{
	(void)as;
	(void)bytes;
	(void)writable;
	if (!va)
		return -1;
	*va = phys;
	service_test_grant_count++;
	return 0;
}

__attribute__((weak)) int zzapp_as_alias_unmap(zzapp_as_t *as, uint32_t va,
						uint32_t bytes)
{
	(void)as;
	(void)va;
	(void)bytes;
	service_test_ungrant_count++;
	return 0;
}


__attribute__((weak)) zzapp_kernel_t *zzapp_runtime_kernel(void)
{
	return &service_test_kernel;
}

__attribute__((weak)) void zzapp_kernel_reset_cpu_ticks(
	zzapp_kernel_t *kernel, uint32_t slot)
{
	if (!kernel || slot >= ZZAPP_KERNEL_APP_SLOTS)
		return;
	kernel->threads[slot].user_ticks = 0U;
	kernel->threads[slot].svc_ticks = 0U;
}

#if ZZAPP_PERF_COUNTERS
__attribute__((weak)) void zzapp_kernel_perf_audio(zzapp_kernel_t *kernel,
						     uint32_t slot,
						     uint32_t low_water,
						     uint32_t starved,
						     uint32_t underruns)
{
	(void)kernel;
	(void)slot;
	(void)low_water;
	(void)starved;
	(void)underruns;
}
#endif

__attribute__((weak)) zzapp_proc_t *zzapp_proc_slot(uint32_t slot)
{
	return slot < ZZAPP_AS_SLOT_COUNT ? &service_test_proc[slot] : NULL;
}

__attribute__((weak)) int zzapp_kernel_current_thread(void)
{
	return service_test_current_thread;
}

__attribute__((weak)) int zzapp_kernel_audio_open(zzapp_kernel_t *kernel,
						    uint32_t slot, int direct)
{
	if (!kernel || slot >= ZZAPP_AS_SLOT_COUNT ||
	    (direct && kernel->direct_audio_count >= 2U))
		return -1;
	if (kernel->resources[slot].direct_audio)
		return -1;
	kernel->resources[slot].direct_audio = direct ? 1U : 2U;
	if (direct)
		kernel->direct_audio_count++;
	return 0;
}

__attribute__((weak)) void zzapp_kernel_audio_close(zzapp_kernel_t *kernel,
						      uint32_t slot)
{
	if (!kernel || slot >= ZZAPP_AS_SLOT_COUNT)
		return;
	if (kernel->resources[slot].direct_audio == 1U &&
	    kernel->direct_audio_count != 0U)
		kernel->direct_audio_count--;
	kernel->resources[slot].direct_audio = 0U;
}
__attribute__((weak)) int zzapp_kernel_wait_status(
	const zzapp_kernel_t *kernel, uint32_t slot)
{
	if (!kernel || slot >= ZZAPP_AS_SLOT_COUNT)
		return ZZAPP_KERNEL_WAIT_CANCELLED;
	if (kernel->threads[slot].wake_reason == ZZAPP_WAKE_TIMEOUT)
		return ZZAPP_KERNEL_WAIT_TIMEOUT;
	if (kernel->threads[slot].wake_reason == ZZAPP_WAKE_CANCELLED)
		return ZZAPP_KERNEL_WAIT_CANCELLED;
	return 0;
}

__attribute__((weak)) int zzapp_kernel_wake(zzapp_kernel_t *kernel,
					      uint32_t slot,
					      zzapp_wake_reason_t reason)
{
	if (!kernel || slot >= ZZAPP_AS_SLOT_COUNT)
		return -1;
	service_test_wake_count++;
	kernel->threads[slot].state = ZZAPP_THREAD_RUNNABLE;
	kernel->threads[slot].wake_reason = reason;
	return 0;
}

__attribute__((weak)) int zzapp_kernel_surface_bind(zzapp_kernel_t *kernel,
						      uint32_t slot,
						      uint32_t generation,
						      uint32_t base,
						      uint32_t bytes)
{
	(void)kernel;
	(void)slot;
	(void)generation;
	return base != 0U && bytes != 0U ? 0 : -1;
}
__attribute__((weak)) void zzapp_kernel_surface_unbind(
	zzapp_kernel_t *kernel, uint32_t slot)
{
	(void)kernel;
	(void)slot;
}



__attribute__((weak)) uint32_t zzapp_runtime_state(void)
{
	return service_loader_state;
}

__attribute__((weak)) void zzapp_loader_set_state(uint32_t state)
{
	service_loader_state = state;
}

__attribute__((weak)) uint32_t zzapp_loader_reset_requested(void)
{
	return 0U;
}
__attribute__((weak)) int zzapp_loader_set_generation(uint32_t generation)
{
	if (service_test_loader_set_generation_hook)
		service_test_loader_set_generation_hook();
	service_loader_generation = generation;
	return generation != 0U ? AAR_OK : AAR_E_INVAL;
}


__attribute__((weak)) int zzapp_loader_begin(void)
{
	if (service_test_loader_begin_hook) {
		service_test_loader_begin_hook();
		return AAR_OK;
	}
	return AAR_E_NOSYS;
}

__attribute__((weak)) int zzapp_loader_chunk(uint32_t file_off, const void *src,
					     uint32_t len)
{
	if (service_test_loader_chunk_hook)
		return service_test_loader_chunk_hook(file_off, src, len);
	return AAR_E_NOSYS;
}

__attribute__((weak)) int zzapp_loader_end(void)
{
	return service_test_loader_end_hook ? service_test_loader_end_hook() :
		AAR_E_NOSYS;
}

__attribute__((weak)) int zzapp_loader_abort(void)
{
	service_test_loader_abort_count++;
	zzapp_services_set_state(ZZAPP_ST_IDLE);
	zzapp_services_loader_aborted();
	return AAR_OK;
}

__attribute__((weak)) int zzapp_loader_alloc_back(uint32_t bytes, uint32_t *addr_out)
{
	(void)bytes;
	(void)addr_out;
	return AAR_E_NOSYS;
}

__attribute__((weak)) void zzapp_loader_free_back(uint32_t addr)
{
	(void)addr;
}
#endif

#ifdef ZZAPP_SERVICES_TEST_LOADER_STUBS
__attribute__((weak)) void zzapp_kernel_reset_cpu_ticks(
	zzapp_kernel_t *kernel, uint32_t slot)
{
	if (!kernel || slot >= ZZAPP_KERNEL_APP_SLOTS)
		return;
	kernel->threads[slot].user_ticks = 0U;
	kernel->threads[slot].svc_ticks = 0U;
}
#endif
#if defined(ZZAPP_HOST_TEST) && !defined(ZZAPP_SERVICES_STANDALONE_TEST)
__attribute__((weak)) int zzapp_loader_abort(void)
{
	return AAR_E_NOSYS;
}
#endif


__attribute__((weak)) void zzapp_host_yield(void)
{
}

__attribute__((weak)) void zzapp_host_yield_wait(void)
{
	zzapp_host_yield();
}

__attribute__((weak)) uint64_t zzapp_host_time_us(void)
{
	return 0U;
}

uint8_t *zzapp_services_window(void)
{
	return have_window() ? current_window() : NULL;
}

void zzapp_services_set_window_mapping_available(int available)
{
	window_mapping_available = available ? 1 : 0;
}

uint32_t zzapp_services_generation(void)
{
	return have_window() ? life_get(ZZAPP_LIFE_GENERATION_OFF) :
	       slots[service_slot()].generation;
}

uint32_t zzapp_services_current_slot(void)
{
	return service_slot();
}

void zzapp_services_select_slot(uint32_t slot)
{
	select_slot(slot);
}

uint32_t zzapp_services_state(void)
{
	uint32_t slot = service_slot();

	if (slot == 0U && slots[0].state != ZZAPP_ST_CLAIMED &&
	    slots[0].state != ZZAPP_ST_STOPPING)
		return zzapp_runtime_state();
	return slots[slot].state;
}

int zzapp_services_stop_requested(void)
{
	return life_get(ZZAPP_LIFE_STOP_OFF) != 0U;
}

static void set_state_locked(uint32_t state)
{
	slots[service_slot()].state = state;
	zzapp_loader_set_state(state);
	life_put(ZZAPP_LIFE_STATE_OFF, state);
}

/* Caller holds g_slot_state_lock. */
static void apply_state_locked(uint32_t state)
{
	struct zzapp_deferred *defer = current_deferred();

	if (state == ZZAPP_ST_IDLE && slots[service_slot()].quarantined) {
		slots[service_slot()].state = ZZAPP_ST_STOPPING;
		life_put(ZZAPP_LIFE_STATE_OFF, ZZAPP_ST_STOPPING);
	} else if (state == ZZAPP_ST_IDLE && defer->cancelled &&
		   defer->state == DEFER_RUNNING) {
		zzapp_loader_set_state(state);
	} else {
		set_state_locked(state);
		if (state == ZZAPP_ST_IDLE)
			/* A finished teardown releases the stop latch with the
			 * slot; quarantines keep it until their release. */
			life_put(ZZAPP_LIFE_STOP_OFF, 0U);
	}
}

void zzapp_services_set_state(uint32_t state)
{
	slot_state_lock_acquire();
	apply_state_locked(state);
	slot_state_lock_release();
}

/*
 * Core 1's teardown publication for the claim it started with.  A moved
 * generation means that claim was already completed and the slot may have
 * been claimed again from core 0, so the newer owner's state stays put.
 */
void zzapp_services_set_idle_for(uint32_t generation)
{
	slot_state_lock_acquire();
	if (slots[service_slot()].generation == generation)
		apply_state_locked(ZZAPP_ST_IDLE);
	slot_state_lock_release();
}


uint32_t zzapp_services_memory_limit(void)
{
	return slots[service_slot()].memory_limit;
}

uint32_t zzapp_services_host_visible_limit(uint32_t slot)
{
	return slot < ZZAPP_SLOT_COUNT ? slots[slot].host_visible_limit : 0U;
}

void zzapp_services_set_fault(uint32_t code, uint32_t pc, uint32_t sp,
			      uint32_t lr, uint32_t far, uint32_t fsr,
			      uint32_t cpsr)
{
	life_put(ZZAPP_LIFE_FAULT_OFF, code);
	life_put(ZZAPP_LIFE_PC_OFF, pc);
	life_put(ZZAPP_LIFE_SP_OFF, sp);
	life_put(ZZAPP_LIFE_LR_OFF, lr);
	life_put(ZZAPP_LIFE_FAR_OFF, far);
	life_put(ZZAPP_LIFE_FSR_OFF, fsr);
	life_put(ZZAPP_LIFE_CPSR_OFF, cpsr);
}

void zzapp_services_set_generation(uint32_t generation)
{
	slot_state_lock_acquire();
	slots[service_slot()].generation = generation;
	life_put(ZZAPP_LIFE_GENERATION_OFF, generation);
	slot_state_lock_release();
}
void zzapp_services_set_exit(int32_t code)
{
	life_put(ZZAPP_LIFE_EXIT_OFF, (uint32_t)code);
}

void zzapp_services_note_progress(uint32_t lo, uint32_t hi)
{
	life_put(ZZAPP_LIFE_PROGRESS_LO_OFF, lo);
	life_put(ZZAPP_LIFE_PROGRESS_HI_OFF, hi);
}

void zzapp_services_or_flags(uint32_t flags)
{
	life_put(ZZAPP_LIFE_FLAGS_OFF, life_get(ZZAPP_LIFE_FLAGS_OFF) | flags);
}

void zzapp_services_and_flags(uint32_t mask)
{
	life_put(ZZAPP_LIFE_FLAGS_OFF, life_get(ZZAPP_LIFE_FLAGS_OFF) & mask);
}

uint32_t zzapp_services_flags(void)
{
	return life_get(ZZAPP_LIFE_FLAGS_OFF);
}

void zzapp_services_set_terminal(uint32_t reason)
{
	slots[service_slot()].terminal_reason = reason;
	life_put(ZZAPP_LIFE_TERMINAL_OFF, reason);
}

static void begin_claim_cancellation(struct zzapp_service_slot *entry,
				     uint32_t reason)
{
	entry->generation++;
	if (entry->generation == 0U)
		entry->generation = 1U;
	life_put(ZZAPP_LIFE_GENERATION_OFF, entry->generation);
	entry->memory_limit = 0U;
	entry->host_visible_limit = 0U;
	entry->load_progress = 0U;
	entry->state = ZZAPP_ST_STOPPING;
	entry->terminal_reason = reason;
	life_put(ZZAPP_LIFE_STOP_OFF, 1U);
	life_put(ZZAPP_LIFE_STATE_OFF, ZZAPP_ST_STOPPING);
	life_put(ZZAPP_LIFE_TERMINAL_OFF, reason);
}

static void publish_claim_idle(struct zzapp_service_slot *entry)
{
	if (entry->quarantined) {
		entry->state = ZZAPP_ST_STOPPING;
		life_put(ZZAPP_LIFE_STATE_OFF, ZZAPP_ST_STOPPING);
		return;
	}
	entry->state = ZZAPP_ST_IDLE;
	life_put(ZZAPP_LIFE_STOP_OFF, 0U);
	life_put(ZZAPP_LIFE_STATE_OFF, ZZAPP_ST_IDLE);
}

/* Caller holds g_slot_state_lock. */
static int expire_current_deferred(void)
{
	struct zzapp_deferred *defer = current_deferred();

	if (defer->state == DEFER_RUNNING)
		return cancel_running_current_deferred();
	if (defer->state == DEFER_DONE)
		return -1;
	if (defer->state != DEFER_IDLE) {
		defer->state = DEFER_IDLE;
		if (deferred_harvest_slot == service_slot())
			deferred_harvest_slot = ZZAPP_SLOT_COUNT;
	}
	return 0;
}

/* Caller holds g_slot_state_lock.  Core 1 keeps a RUNNING record and
 * completes its cancellation (nonzero return); a queued or finished one is
 * dropped. */
static int discard_current_deferred(void)
{
	int running = expire_current_deferred();

	if (running < 0) {
		current_deferred()->state = DEFER_IDLE;
		if (deferred_harvest_slot == service_slot())
			deferred_harvest_slot = ZZAPP_SLOT_COUNT;
	}
	return running > 0;
}

static void renew_launcher_lease(struct zzapp_service_slot *entry, uint64_t now)
{
	uint32_t heartbeat = life_get(ZZAPP_LIFE_HEARTBEAT_OFF);

	if (heartbeat == entry->launcher_heartbeat)
		return;
	entry->launcher_heartbeat = heartbeat;
	entry->claim_started = now;
	entry->load_progress = now;
}

void zzapp_services_expire_claims(uint64_t now)
{
	uint32_t saved = selected_slot[service_context()];
	uint32_t slot;

	for (slot = 0U; slot < ZZAPP_SLOT_COUNT; slot++) {
		struct zzapp_service_slot *entry = &slots[slot];
		uint64_t deadline;
		int loading;

		select_slot(slot);
		slot_state_lock_acquire();
		if (entry->state == ZZAPP_ST_CLAIMED) {
			renew_launcher_lease(entry, now);
			deadline = entry->claim_started + ZZAPP_CLAIM_LEASE_US;
			loading = current_deferred()->opcode == ZZAPP_OP_LOAD_BEGIN &&
				  current_deferred()->state == DEFER_RUNNING;
		} else if (entry->state == ZZAPP_ST_LOADING) {
			renew_launcher_lease(entry, now);
			deadline = entry->load_progress + ZZAPP_LOAD_LEASE_US;
			loading = 1;
		} else {
			slot_state_lock_release();
			continue;
		}
		if (now < deadline) {
			slot_state_lock_release();
			continue;
		}
		discard_current_deferred();
		if (loading) {
			entry->quarantined = 1;
			entry->quarantine_reclaimed = 0;
			entry->quarantine_release_requested = 0;
			begin_claim_cancellation(entry, ZZAPP_END_LEASE);
			entry->terminal_reason = ZZAPP_END_QUARANTINED;
			life_put(ZZAPP_LIFE_TERMINAL_OFF, ZZAPP_END_QUARANTINED);
		} else {
			begin_claim_cancellation(entry, ZZAPP_END_LEASE);
			memset(current_runtime(), 0, sizeof(*current_runtime()));
			publish_claim_idle(entry);
		}
		slot_state_lock_release();
	}
	select_slot(saved);
}

static int grant_user_buffer(uint32_t address, uint32_t bytes,
			     uint32_t *grant_base, uint32_t *grant_bytes,
			     uint32_t *user_address)
{
	zzapp_proc_t *proc = zzapp_proc_slot(service_slot());
	uint32_t base;
	uint32_t alias;
	uint64_t end;

	if (!proc || bytes == 0U)
		return AAR_E_NOMEM;
	base = address & ~0xfffU;
	end = ((uint64_t)address + bytes + 0xfffU) & ~0xfffULL;
	if (end > (uint64_t)UINT32_MAX + 1U || end <= base ||
	    zzapp_as_alias_map(&proc->as, base, (uint32_t)(end - base), 1,
				 &alias) != 0)
		return AAR_E_NOMEM;
	*grant_base = alias;
	*grant_bytes = (uint32_t)(end - base);
	*user_address = alias + address - base;
	return AAR_OK;
}

static void revoke_surface_grants(void)
{
	struct zzapp_service_runtime *state = current_runtime();
	zzapp_proc_t *proc = zzapp_proc_slot(service_slot());
	uint32_t i;

	for (i = 0U; i < 3U; i++) {
		if (proc && state->display_grant_bytes[i] != 0U)
			(void)zzapp_as_alias_unmap(&proc->as,
						    state->display_grant_base[i],
						    state->display_grant_bytes[i]);
		state->display_grant_base[i] = 0U;
		state->display_grant_bytes[i] = 0U;
		state->display_alias_phys[i] = 0U;
		state->display_alias[i] = 0U;
	}
}

static int grant_surface_buffers(uint32_t front, uint32_t back,
				 uint32_t spare, uint32_t bytes)
{
	struct zzapp_service_runtime *state = current_runtime();
	uint32_t buffers[3] = { front, back, spare };
	uint32_t i;

	for (i = 0U; i < 3U; i++) {
		if (buffers[i] == 0U)
			continue;
		if (grant_user_buffer(buffers[i], bytes,
				      &state->display_grant_base[i],
				      &state->display_grant_bytes[i],
				      &state->display_alias[i]) != AAR_OK) {
			revoke_surface_grants();
			return AAR_E_NOMEM;
		}
		state->display_alias_phys[i] = buffers[i];
	}
	return AAR_OK;
}
static void release_unbound_back(uint32_t back)
{
	if (!current_runtime()->display_back_owned)
		return;
	/* Ownership is dropped before the block is given back, see
	 * forget_window. */
	current_runtime()->display_back_owned = 0;
	zzapp_loader_free_back(back);
}


#ifdef ZZAPP_HOST_TEST
/* Stands in for the surface allocator's fixed window range of each slot: the
 * backing array the test injected, and whether its range is still allocated.
 * A test that installs an allocator model (zzapp_test_card_alloc_at and
 * _free) gets ownership by address and serial instead of the flag. */
static uint8_t *test_window_backing[ZZAPP_SLOT_COUNT];
static int test_window_range_held[ZZAPP_SLOT_COUNT];
static uint32_t test_window_serial[ZZAPP_SLOT_COUNT];
extern uint32_t zzapp_test_card_alloc_at(uint32_t addr, uint32_t bytes,
					 uint32_t *serial) __attribute__((weak));
extern void zzapp_test_card_free(uint32_t card, uint32_t serial)
	__attribute__((weak));
#endif

/* The slot's window is a block of the surface allocator, at a fixed range.
 * Forgetting it without freeing the block leaves that range occupied, and
 * the slot's next claim asks the allocator for the same range again. */
static void release_window_range(uint32_t slot, uint8_t *base,
				 uint32_t serial)
{
	if (!base)
		return;
#ifdef ZZAPP_HOST_TEST
	if (zzapp_test_card_free)
		zzapp_test_card_free(ZZAPP_WINDOW_Z3_SLOT_ADDRESS(slot), serial);
	else
		test_window_range_held[slot] = 0;
#else
	(void)slot;
	(void)serial;
	(void)surface_allocator_free((uint32_t)(uintptr_t)base);
#endif
}

static void forget_window(void)
{
	uint32_t slot = service_slot();
	struct zzapp_service_runtime *state = current_runtime();
	struct zzapp_service_runtime saved = *state;

	uint8_t *base = slots[slot].window;
#ifdef ZZAPP_HOST_TEST
	uint32_t serial = test_window_serial[slot];
#else
	uint32_t serial = 0U;
#endif

	/* The window is detached from the slot before its block is freed. A
	 * core 1 restart can replay this teardown, and the allocator frees by
	 * address: a pointer that outlived its free would take the block of
	 * whoever was given the address meanwhile. A halt in between leaks the
	 * block until the allocator is reset. */
	slots[slot].window = NULL;
	release_window_range(slot, base, serial);
	slots[slot].window_bytes = 0U;
	slots[slot].window_uncached = 0;
	memset(state, 0, sizeof(*state));
	if (!saved.audio_open && !saved.audio_block &&
	    saved.audio_request != AUDIO_REQUEST_CLOSE &&
	    saved.audio_request != AUDIO_REQUEST_CLOSE_QUIESCED)
		return;
	state->audio_open = saved.audio_open;
	state->audio_slot = saved.audio_slot;
	state->audio_read_frame = saved.audio_read_frame;
	state->audio_write_frame = saved.audio_write_frame;
	state->audio_samples = saved.audio_samples;
	state->audio_staged = saved.audio_staged;
	state->audio_ahi_staged_frame = saved.audio_ahi_staged_frame;
	state->audio_first_write_seen = saved.audio_first_write_seen;
	state->audio_block = saved.audio_block;
	state->audio_grant_base = saved.audio_grant_base;
	state->audio_grant_bytes = saved.audio_grant_bytes;
	state->audio_alias = saved.audio_alias;
	state->audio_request = saved.audio_request;
	state->audio_result = saved.audio_result;
	state->audio_teardown = saved.audio_teardown;
	state->audio_stream_epoch = saved.audio_stream_epoch;
	state->audio_ahi_route = saved.audio_ahi_route;
	state->audio_host_tail_seen = saved.audio_host_tail_seen;
#ifdef ZZAPP_HOST_TEST
	state->audio_detach_count = saved.audio_detach_count;
#endif
}

/* Amiga reset: a slot that is idle still holds its window. The allocator is
 * reset right after, so the block is given back and the pointer dropped now;
 * claims allocate a new window. */
void zzapp_services_retire_idle_window(void)
{
	if (slots[service_slot()].state == ZZAPP_ST_IDLE && have_window())
		forget_window();
}

static void audio_detach(void);
static void audio_post_teardown(void);

void zzapp_services_on_launch(void)
{
	struct zzapp_service_runtime *state = current_runtime();

	state->log_ring_count = 0U;
	state->log_ring_window = 0U;
	load_diag_seen = 0U;
	if (have_window()) {
		memset(current_window() + ZZAPP_CONSOLE_OUT_OFF, 0, ZZAPP_CONSOLE_HDR);
		memset(current_window() + ZZAPP_CONSOLE_IN_OFF, 0, ZZAPP_CONSOLE_HDR);
		memset(current_window() + ZZAPP_AUDIO_OFF, 0, ZZAPP_AUDIO_HDR);
		memset(current_window() + ZZAPP_MSG_H2A_OFF, 0, ZZAPP_MSG_HDR);
		memset(current_window() + ZZAPP_MSG_A2H_OFF, 0, ZZAPP_MSG_HDR);
		win_flush(ZZAPP_CONSOLE_OUT_OFF, ZZAPP_CONSOLE_HDR);
		win_flush(ZZAPP_CONSOLE_IN_OFF, ZZAPP_CONSOLE_HDR);
		win_flush(ZZAPP_AUDIO_OFF, ZZAPP_AUDIO_HDR);
		win_flush(ZZAPP_MSG_H2A_OFF, ZZAPP_MSG_HDR);
		win_flush(ZZAPP_MSG_A2H_OFF, ZZAPP_MSG_HDR);
	}
#ifndef ZZAPP_HOST_TEST
	state->log_uart_count = 0U;
	state->log_uart_window = 0U;
#endif
}

void zzapp_services_on_app_end(void)
{
	struct zzapp_service_runtime *state = current_runtime();

	audio_post_teardown();
	revoke_surface_grants();
	zzapp_kernel_surface_unbind(zzapp_runtime_kernel(), service_slot());
	if (state->display_back_owned)
		zzapp_loader_free_back(life_get(ZZAPP_LIFE_CARD_BACK_OFF));
	state->display_bound = 0;
	state->display_back_owned = 0;
	state->display_id = 0U;
	state->display_ack_seen = 0U;
	state->display_queued = 0U;
	display_mailbox_disable();
	wait_end();
	memset(state->file_open, 0, sizeof(state->file_open));
	memset(state->list_open, 0, sizeof(state->list_open));
	zzapp_services_and_flags(~(ZZAPP_F_DISPLAY_UNBOUND | ZZAPP_F_VSYNC |
				   ZZAPP_F_SCANOUT_ACK | ZZAPP_F_MAILBOX));
	life_put(ZZAPP_LIFE_CARD_FRONT_OFF, 0U);
	life_put(ZZAPP_LIFE_CARD_BACK_OFF, 0U);
	life_put(ZZAPP_LIFE_FLIP_ADDRESS_OFF, 0U);
	life_put(ZZAPP_LIFE_FLIP_SUBMIT_OFF, 0U);
	life_put(ZZAPP_LIFE_MAILBOX_REPLACED_OFF, 0U);
	slot_state_lock_acquire();
	if (slots[service_slot()].quarantined || state->audio_teardown ||
	    state->audio_request == AUDIO_REQUEST_CLOSE ||
	    state->audio_request == AUDIO_REQUEST_CLOSE_QUIESCED) {
		slots[service_slot()].state = ZZAPP_ST_STOPPING;
		life_put(ZZAPP_LIFE_STATE_OFF, ZZAPP_ST_STOPPING);
	} else {
		slots[service_slot()].state = ZZAPP_ST_IDLE;
	}
	slot_state_lock_release();
}

void zzapp_services_on_reset(void)
{
	zzapp_services_on_app_end();
#ifndef ZZAPP_HOST_TEST
	printf("[app] reset: window forgotten\r\n");
#endif
	forget_window();
	slot_state_lock_acquire();
	slots[service_slot()].quarantined = 0;
	slots[service_slot()].quarantine_reclaimed = 0;
	slots[service_slot()].quarantine_release_requested = 0;
	if (discard_current_deferred()) {
		/* Core 1 still owns the cancelled record; completing it is the
		 * admission point, so the slot reads STOPPING until then. */
		slots[service_slot()].state = ZZAPP_ST_STOPPING;
		life_put(ZZAPP_LIFE_STATE_OFF, ZZAPP_ST_STOPPING);
	} else {
		publish_claim_idle(&slots[service_slot()]);
	}
	current_deferred()->start_pending = 0U;
	slot_state_lock_release();
	zzapp_services_on_launch();
}

void zzapp_services_loader_aborted(void)
{
	struct zzapp_service_slot *entry = &slots[service_slot()];

	slot_state_lock_acquire();
	if (entry->quarantined) {
		memset(current_runtime(), 0, sizeof(*current_runtime()));
		entry->quarantine_reclaimed = 1;
		set_state_locked(ZZAPP_ST_STOPPING);
		life_put(ZZAPP_LIFE_TERMINAL_OFF, ZZAPP_END_QUARANTINED);
		if (entry->quarantine_release_requested)
			release_quarantine_locked(entry);
	}
	slot_state_lock_release();
}

static int fd_index(int fd)
{
	if (fd < ZZAPP_FD_BASE || fd >= ZZAPP_FD_BASE + ZZAPP_FD_COUNT)
		return -1;
	return fd - ZZAPP_FD_BASE;
}

static int alloc_fd(int *table)
{
	int i;

	for (i = 0; i < ZZAPP_FD_COUNT; i++) {
		if (!table[i]) {
			table[i] = 1;
			return ZZAPP_FD_BASE + i;
		}
	}
	return -1;
}

/* 1 is a live fd. 2 is retired by a timeout: ZZRun may still complete the
 * abandoned op into that index, so it must not be allocated again. */
static int slot_live(const int *table, int fd)
{
	int ix = fd_index(fd);

	return ix >= 0 && table[ix] == 1;
}

static void release_slot(int *table, int fd, int timed_out)
{
	int ix = fd_index(fd);

	if (ix >= 0)
		table[ix] = timed_out ? 2 : 0;
}

static uint16_t zz_to_sdk(int zz)
{
	switch (zz) {
	case AAR_OK:
		return SDK_STATUS_OK;
	case AAR_E_INVAL:
	case AAR_E_ACCES:
	case AAR_E_BADF:
	case AAR_E_PROFILE:
		return SDK_STATUS_BAD_REQUEST;
	case AAR_E_NOGROUP:
		return SDK_STATUS_UNSUPPORTED;
	case AAR_E_NOMEM:
		return SDK_STATUS_NO_MEMORY;
	case AAR_E_BUSY:
		return SDK_STATUS_BUSY;
	case AAR_E_NOENT:
		return SDK_STATUS_NOT_FOUND;
	case AAR_E_NOSYS:
		return SDK_STATUS_UNSUPPORTED;
	default:
		return SDK_STATUS_IO_ERROR;
	}
}

static int publish_file(uint32_t op, int fd, int32_t flags, int32_t whence,
			uint32_t path_off, uint32_t length, uint64_t offset,
			uint32_t mode)
{
	struct zzapp_service_runtime *state = current_runtime();
	uint32_t odd;
	uint32_t even;

	if (!have_window())
		return AAR_E_IO;
	odd = state->file_seq + 1U;
	even = state->file_seq + 2U;
	win_put32(ZZAPP_FILE_OFF + 0U, odd);
	barrier();
	win_put32(ZZAPP_FILE_OFF + 4U, op);
	win_put32(ZZAPP_FILE_OFF + 8U, (uint32_t)fd);
	win_put32(ZZAPP_FILE_OFF + 12U, (uint32_t)flags);
	win_put32(ZZAPP_FILE_OFF + 16U, (uint32_t)whence);
	win_put32(ZZAPP_FILE_OFF + 20U, 0U);
	win_put32(ZZAPP_FILE_OFF + 24U, path_off);
	win_put32(ZZAPP_FILE_OFF + 28U, length);
	win_put32(ZZAPP_FILE_OFF + 32U, 0U);
	win_put32(ZZAPP_FILE_OFF + 36U, mode);
	zzapp_put_be64(current_window() + ZZAPP_FILE_OFF + 40U, offset);
	win_flush(ZZAPP_FILE_OFF + 40U, 8U);
	zzapp_put_be64(current_window() + ZZAPP_FILE_OFF + 48U, 0U);
	win_flush(ZZAPP_FILE_OFF + 48U, 8U);
	win_put32(ZZAPP_FILE_OFF + 56U, 0U);
	win_put32(ZZAPP_FILE_OFF + 60U, 0U);
	barrier();
	win_put32(ZZAPP_FILE_OFF + 0U, even);
	state->file_seq = even;
	return AAR_OK;
}

static int wait_file(uint32_t *out_n, uint64_t *out_pos, uint32_t *mode,
		     uint32_t *mtime, int32_t *err)
{
	struct zzapp_service_runtime *state = current_runtime();
	uint32_t generation = zzapp_services_generation();
	uint64_t start = now_us();
	uint32_t expect = state->file_seq + 2U;
	wait_begin(ZZAPP_WAIT_FILE, expect, start + ZZAPP_FILE_TIMEOUT_US);


	for (;;) {
		uint32_t s1;
		uint32_t s2;
		uint32_t n;
		uint64_t pos;
		uint32_t reply_mode;
		uint32_t reply_mtime;
		int32_t reply_err;

		if (zzapp_services_generation() != generation ||
		    zzapp_services_stop_requested())
			return AAR_E_IO;
		win_inval(ZZAPP_FILE_OFF, ZZAPP_FILE_BYTES);
		barrier();
		s1 = win_get32(ZZAPP_FILE_OFF + 0U);
		if (s1 == expect) {
			n = win_get32(ZZAPP_FILE_OFF + 32U);
			pos = ((uint64_t)win_get32(ZZAPP_FILE_OFF + 48U) << 32) |
			      win_get32(ZZAPP_FILE_OFF + 52U);
			reply_mode = win_get32(ZZAPP_FILE_OFF + 36U);
			reply_mtime = win_get32(ZZAPP_FILE_OFF + 56U);
			reply_err = (int32_t)win_get32(ZZAPP_FILE_OFF + 20U);
			barrier();
			s2 = win_get32(ZZAPP_FILE_OFF + 0U);
			if (s1 == s2 && zzapp_services_generation() == generation) {
				if (out_n)
					*out_n = n;
				if (out_pos)
					*out_pos = pos;
				if (mode)
					*mode = reply_mode;
				if (mtime)
					*mtime = reply_mtime;
				if (err)
					*err = reply_err;
				state->file_seq = s1;
				wait_end();
				return AAR_OK;
			}
		}
		if (now_us() - start >= ZZAPP_FILE_TIMEOUT_US) {
			win_put32(ZZAPP_FILE_OFF + 0U, state->file_seq + 1U);
			barrier();
			win_put32(ZZAPP_FILE_OFF + 4U, 0U);
			state->file_seq = expect;
			wait_end();
			return AAR_E_TIMEDOUT;
		}
		yield_wait_once();
		if (wait_status() == AAR_E_TIMEDOUT) {
			win_put32(ZZAPP_FILE_OFF + 0U, state->file_seq + 1U);
			barrier();
			win_put32(ZZAPP_FILE_OFF + 4U, 0U);
			state->file_seq = expect;
			wait_end();
			return AAR_E_TIMEDOUT;
		}
		if (wait_status() != AAR_OK) {
			wait_end();
			return wait_status();
		}
}
}


static int put_path(const char *path, uint32_t *path_off)
{
	size_t n;

	if (!path)
		return AAR_E_INVAL;
	n = strlen(path);
	if (n == 0U || n > ZZAPP_PATH_MAX)
		return AAR_E_INVAL;
	memcpy(current_window() + ZZAPP_PATH_OFF, path, n + 1U);
	win_flush(ZZAPP_PATH_OFF, (uint32_t)(n + 1U));
	*path_off = ZZAPP_PATH_OFF;
	return AAR_OK;
}

/* AAR file open flags (spec 7.1). At least one of READ and WRITE; CREATE
 * and TRUNCATE need WRITE; EXCL needs CREATE; no other bit. */
static int file_flags_valid(uint32_t flags)
{
	if ((flags & ~(ZZAPP_FILE_READ | ZZAPP_FILE_WRITE | ZZAPP_FILE_CREATE |
		       ZZAPP_FILE_TRUNCATE | ZZAPP_FILE_EXCL)) != 0U)
		return 0;
	if ((flags & (ZZAPP_FILE_READ | ZZAPP_FILE_WRITE)) == 0U)
		return 0;
	if ((flags & (ZZAPP_FILE_CREATE | ZZAPP_FILE_TRUNCATE)) != 0U &&
	    (flags & ZZAPP_FILE_WRITE) == 0U)
		return 0;
	if ((flags & ZZAPP_FILE_EXCL) != 0U && (flags & ZZAPP_FILE_CREATE) == 0U)
		return 0;
	return 1;
}

int zzapp_svc_file_open(const char *path, uint32_t flags)
{
	struct zzapp_service_runtime *state = current_runtime();
	uint32_t path_off = 0U;
	uint32_t mode = 0U;
	int32_t err = 0;
	int fd;
	int rc;

	if (!have_window())
		return AAR_E_IO;
	if (!file_flags_valid(flags))
		return AAR_E_INVAL;
	rc = put_path(path, &path_off);
	if (rc != AAR_OK)
		return rc;
	fd = alloc_fd(state->file_open);
	if (fd < 0)
		return AAR_E_NOMEM;
	rc = publish_file(ZZAPP_FILE_OP_OPEN, fd, (int32_t)flags, 0, path_off,
			  0U, 0U, 0U);
	if (rc != AAR_OK) {
		state->file_open[fd_index(fd)] = 0;
		return rc;
	}
	rc = wait_file(NULL, NULL, &mode, NULL, &err);
	if (rc != AAR_OK || err != 0) {
		release_slot(state->file_open, fd, rc == AAR_E_TIMEDOUT);
		return rc != AAR_OK ? rc : err;
	}
	(void)mode;
	return fd;
}

int zzapp_svc_file_close(int fd)
{
	struct zzapp_service_runtime *state = current_runtime();
	int32_t err = 0;
	int rc;

	if (!slot_live(state->file_open, fd))
		return AAR_E_BADF;
	rc = publish_file(ZZAPP_FILE_OP_CLOSE, fd, 0, 0, 0U, 0U, 0U, 0U);
	if (rc != AAR_OK)
		return rc;
	rc = wait_file(NULL, NULL, NULL, NULL, &err);
	release_slot(state->file_open, fd, rc == AAR_E_TIMEDOUT);
	if (rc != AAR_OK)
		return rc;
	return err;
}

int zzapp_svc_file_read(int fd, void *buf, uint32_t len, uint32_t *out_n)
{
	struct zzapp_service_runtime *state = current_runtime();
	uint8_t *dst = (uint8_t *)buf;
	uint32_t got = 0U;

	if (!out_n)
		return AAR_E_INVAL;
	*out_n = 0U;
	if (!slot_live(state->file_open, fd) || !dst)
		return AAR_E_BADF;
	while (got < len) {
		uint32_t chunk = len - got;
		uint32_t n = 0U;
		int32_t err = 0;
		int rc;

		if (chunk > ZZAPP_DATA_BYTES)
			chunk = ZZAPP_DATA_BYTES;
		rc = publish_file(ZZAPP_FILE_OP_READ, fd, 0, 0, 0U, chunk, 0U, 0U);
		if (rc != AAR_OK)
			return rc;
		rc = wait_file(&n, NULL, NULL, NULL, &err);
		if (rc != AAR_OK)
			return rc;
		if (err != 0)
			return err;
		if (n > chunk)
			n = chunk;
		if (n != 0U) {
			win_inval(ZZAPP_DATA_OFF, n);
			memcpy(dst + got, current_window() + ZZAPP_DATA_OFF, n);
		}
		got += n;
		*out_n = got;
		if (n < chunk)
			break;
	}
	return AAR_OK;
}

int zzapp_svc_file_write(int fd, const void *buf, uint32_t len, uint32_t *out_n)
{
	struct zzapp_service_runtime *state = current_runtime();
	const uint8_t *src = (const uint8_t *)buf;
	uint32_t got = 0U;

	if (!out_n)
		return AAR_E_INVAL;
	*out_n = 0U;
	if (!slot_live(state->file_open, fd) || (!src && len != 0U))
		return AAR_E_BADF;
	while (got < len) {
		uint32_t chunk = len - got;
		uint32_t n = 0U;
		int32_t err = 0;
		int rc;

		if (chunk > ZZAPP_DATA_BYTES)
			chunk = ZZAPP_DATA_BYTES;
		memcpy(current_window() + ZZAPP_DATA_OFF, src + got, chunk);
		win_flush(ZZAPP_DATA_OFF, chunk);
		rc = publish_file(ZZAPP_FILE_OP_WRITE, fd, 0, 0, 0U, chunk, 0U, 0U);
		if (rc != AAR_OK)
			return rc;
		rc = wait_file(&n, NULL, NULL, NULL, &err);
		if (rc != AAR_OK)
			return rc;
		if (err != 0)
			return err;
		if (n > chunk)
			n = chunk;
		got += n;
		*out_n = got;
		if (n < chunk)
			break;
	}
	return AAR_OK;
}

int zzapp_svc_file_seek(int fd, int64_t off, int whence, int64_t *out_pos)
{
	struct zzapp_service_runtime *state = current_runtime();
	uint64_t pos = 0U;
	int32_t err = 0;
	int rc;

	if (!slot_live(state->file_open, fd))
		return AAR_E_BADF;
	if (whence != ZZAPP_SEEK_SET && whence != ZZAPP_SEEK_CUR &&
	    whence != ZZAPP_SEEK_END)
		return AAR_E_INVAL;
	rc = publish_file(ZZAPP_FILE_OP_SEEK, fd, 0, whence, 0U, 0U,
			  (uint64_t)off, 0U);
	if (rc != AAR_OK)
		return rc;
	rc = wait_file(NULL, &pos, NULL, NULL, &err);
	if (rc != AAR_OK)
		return rc;
	if (err != 0)
		return err;
	if (out_pos)
		*out_pos = (int64_t)pos;
	return AAR_OK;
}

int zzapp_svc_file_stat(const char *path, aar_stat *out)
{
	uint32_t path_off = 0U;
	uint64_t size = 0U;
	uint32_t mode = 0U;
	uint32_t mtime = 0U;
	int32_t err = 0;
	int rc;

	if (!out)
		return AAR_E_INVAL;
	rc = put_path(path, &path_off);
	if (rc != AAR_OK)
		return rc;
	rc = publish_file(ZZAPP_FILE_OP_STAT, 0, 0, 0, path_off, 0U, 0U, 0U);
	if (rc != AAR_OK)
		return rc;
	rc = wait_file(NULL, &size, &mode, &mtime, &err);
	if (rc != AAR_OK)
		return rc;
	if (err != 0)
		return err;
	out->size = size;
	out->mtime = mtime;
	out->mode = mode;
	out->reserved = 0U;
	return AAR_OK;
}

int zzapp_svc_file_list_open(const char *path)
{
	struct zzapp_service_runtime *state = current_runtime();
	uint32_t path_off = 0U;
	int32_t err = 0;
	int list;
	int rc;

	rc = put_path(path, &path_off);
	if (rc != AAR_OK)
		return rc;
	list = alloc_fd(state->list_open);
	if (list < 0)
		return AAR_E_NOMEM;
	rc = publish_file(ZZAPP_FILE_OP_LIST_OPEN, list, 0, 0, path_off, 0U, 0U, 0U);
	if (rc != AAR_OK) {
		state->list_open[fd_index(list)] = 0;
		return rc;
	}
	rc = wait_file(NULL, NULL, NULL, NULL, &err);
	if (rc != AAR_OK || err != 0) {
		release_slot(state->list_open, list, rc == AAR_E_TIMEDOUT);
		return rc != AAR_OK ? rc : err;
	}
	return list;
}

int zzapp_svc_file_list_next(int list, aar_dirent *out)
{
	struct zzapp_service_runtime *state = current_runtime();
	uint32_t n = 0U;
	uint32_t mode = 0U;
	int32_t err = 0;
	int rc;

	if (!slot_live(state->list_open, list) || !out)
		return AAR_E_BADF;
	rc = publish_file(ZZAPP_FILE_OP_LIST_NEXT, list, 0, 0, 0U, 0U, 0U, 0U);
	if (rc != AAR_OK)
		return rc;
	rc = wait_file(&n, NULL, &mode, NULL, &err);
	if (rc != AAR_OK)
		return rc;
	if (err != 0)
		return err;
	if (n == 0U)
		return AAR_E_NOENT;
	if (n > 255U)
		n = 255U;
	win_inval(ZZAPP_DATA_OFF, n + 1U);
	memcpy(out->name, current_window() + ZZAPP_DATA_OFF, n);
	out->name[n] = '\0';
	out->mode = mode;
	return AAR_OK;
}

int zzapp_svc_file_list_close(int list)
{
	struct zzapp_service_runtime *state = current_runtime();
	int32_t err = 0;
	int rc;

	if (!slot_live(state->list_open, list))
		return AAR_E_BADF;
	rc = publish_file(ZZAPP_FILE_OP_LIST_CLOSE, list, 0, 0, 0U, 0U, 0U, 0U);
	if (rc != AAR_OK)
		return rc;
	rc = wait_file(NULL, NULL, NULL, NULL, &err);
	release_slot(state->list_open, list, rc == AAR_E_TIMEDOUT);
	return rc != AAR_OK ? rc : err;
}

static uint32_t log_usable(void)
{
	return ZZAPP_LOG_BYTES - ZZAPP_LOG_HDR;
}

static void log_drop(void)
{
	life_put(ZZAPP_LIFE_LOG_DROPS_OFF, life_get(ZZAPP_LIFE_LOG_DROPS_OFF) + 1U);
}

void zzapp_svc_log(const char *msg, uint32_t len)
{
	uint32_t usable;
	uint32_t rec;
	uint32_t head;
	uint32_t tail;
	uint32_t pos;
	uint32_t i;
	uint64_t t;
	uint8_t hdr[2];

	if (!have_window() || !msg)
		return;
	if (len > 1024U)
		len = 1024U;
	t = now_us();
	if (t - current_runtime()->log_ring_window >= 1000000ull) {
		current_runtime()->log_ring_window = t;
		current_runtime()->log_ring_count = 0U;
	}
	if (current_runtime()->log_ring_count >= ZZAPP_LOG_RING_HZ) {
		log_drop();
		return;
	}
	usable = log_usable();
	rec = (2U + len + 3U) & ~3U;
	head = win_get32(ZZAPP_LOG_OFF + 0U);
	tail = win_get32(ZZAPP_LOG_OFF + 4U);
	if (head - tail + rec > usable) {
		log_drop();
		return;
	}
	zzapp_put_be16(hdr, (uint16_t)len);
	pos = head % usable;
	for (i = 0; i < rec; i++) {
		uint8_t b = 0U;
		uint32_t at = (pos + i) % usable;

		if (i < 2U)
			b = hdr[i];
		else if (i < 2U + len)
			b = (uint8_t)msg[i - 2U];
		current_window()[ZZAPP_LOG_OFF + ZZAPP_LOG_HDR + at] = b;
	}
	win_flush(ZZAPP_LOG_OFF + ZZAPP_LOG_HDR, usable);
	barrier();
	win_put32(ZZAPP_LOG_OFF + 0U, head + rec);
	current_runtime()->log_ring_count++;
#ifndef ZZAPP_HOST_TEST
	if (t - current_runtime()->log_uart_window >= 1000000ull) {
		current_runtime()->log_uart_window = t;
		current_runtime()->log_uart_count = 0U;
	}
	if (current_runtime()->log_uart_count < ZZAPP_LOG_UART_HZ) {
		printf("[app] %.*s\r\n", (int)len, msg);
		current_runtime()->log_uart_count++;
	}
#endif
}

int zzapp_svc_should_stop(void)
{
	return zzapp_services_stop_requested();
}

/* aar_event_poll. Lifecycle events (stop, low memory) are always delivered;
 * the launcher's window events only with AAR_GROUP_INPUT. A stop request
 * queues one AAR_EV_STOP whether or not the launcher also posts one. */
int zzapp_svc_event_poll(aar_event *out, uint32_t max, uint32_t *out_n)
{
	uint32_t slot = service_slot();
	uint32_t head;
	uint32_t tail;
	uint32_t n = 0U;

	if (!out_n)
		return AAR_E_INVAL;
	*out_n = 0U;
	if (max != 0U && !out)
		return AAR_E_INVAL;
	if (n < max && zzapp_host_take_stop_event &&
	    zzapp_host_take_stop_event(slot)) {
		memset(&out[n], 0, sizeof(out[n]));
		out[n].type = AAR_EV_STOP;
		n++;
	}
	if (n < max && zzapp_host_take_low_memory_event &&
	    zzapp_host_take_low_memory_event(slot)) {
		memset(&out[n], 0, sizeof(out[n]));
		out[n].type = AAR_EV_LOW_MEMORY;
		n++;
	}
	if ((zzapp_runtime_groups() & AAR_GROUP_INPUT) == 0U || !have_window()) {
		*out_n = n;
		return AAR_OK;
	}
	head = win_get32(ZZAPP_INPUT_OFF + 0U);
	tail = win_get32(ZZAPP_INPUT_OFF + 4U);
	barrier();
	if (head - tail > ZZAPP_INPUT_CAP)
		tail = head - ZZAPP_INPUT_CAP;
	while (n < max && tail != head) {
		uint32_t ring_slot = tail & (ZZAPP_INPUT_CAP - 1U);
		uint32_t off = ZZAPP_INPUT_OFF + ZZAPP_INPUT_HDR +
			       ring_slot * ZZAPP_EVENT_BYTES;
		aar_event *ev = &out[n];

		win_inval(off, ZZAPP_EVENT_BYTES);
		barrier();
		ev->type = zzapp_get_be16(current_window() + off + 0U);
		ev->code = zzapp_get_be16(current_window() + off + 2U);
		ev->dx = zzapp_get_be16s(current_window() + off + 4U);
		ev->dy = zzapp_get_be16s(current_window() + off + 6U);
		ev->buttons = zzapp_get_be16(current_window() + off + 8U);
		ev->reserved = zzapp_get_be16(current_window() + off + 10U);
		ev->time_ms = zzapp_get_be32(current_window() + off + 12U);
		tail++;
		/* The launcher's own stop event is the delivery of the request;
		 * a second one for the same request is dropped. */
		if (ev->type == AAR_EV_STOP && zzapp_host_note_stop_event_delivered &&
		    zzapp_host_note_stop_event_delivered(slot))
			continue;
		n++;
	}
	if (tail != win_get32(ZZAPP_INPUT_OFF + 4U)) {
		barrier();
		win_put32(ZZAPP_INPUT_OFF + 4U, tail);
	}
	*out_n = n;
	return AAR_OK;
}

static uint32_t console_available(uint32_t head, uint32_t tail, uint32_t cap)
{
	return head - tail <= cap ? head - tail : cap;
}

int zzapp_svc_console_write(const void *buf, uint32_t len, uint32_t *out_n)
{
	const uint8_t *src = (const uint8_t *)buf;
	uint32_t done = 0U;

	if (!out_n || (len != 0U && !src))
		return AAR_E_INVAL;
	*out_n = 0U;
	if (!have_window())
		return AAR_E_IO;
	zzapp_services_or_flags(ZZAPP_F_CONSOLE);
	while (done < len) {
		uint32_t head = win_get32(ZZAPP_CONSOLE_OUT_OFF);
		uint32_t tail = win_get32(ZZAPP_CONSOLE_OUT_OFF + 4U);
		uint32_t used = console_available(head, tail, ZZAPP_CONSOLE_OUT_CAP);
		uint32_t at;
		uint32_t n;

		if (head - tail > ZZAPP_CONSOLE_OUT_CAP)
			return AAR_E_IO;
		if (used == ZZAPP_CONSOLE_OUT_CAP) {
			if (zzapp_services_stop_requested())
				return AAR_E_IO;
			wait_begin(ZZAPP_WAIT_CONSOLE_WRITE, tail, 0U);
			yield_wait_once();
			if (wait_status() != AAR_OK) {
				wait_end();
				return wait_status();
			}
			continue;
		}
		at = head % ZZAPP_CONSOLE_OUT_CAP;
		n = ZZAPP_CONSOLE_OUT_CAP - at;
		if (n > ZZAPP_CONSOLE_OUT_CAP - used)
			n = ZZAPP_CONSOLE_OUT_CAP - used;
		if (n > len - done)
			n = len - done;
		memcpy(current_window() + ZZAPP_CONSOLE_OUT_OFF + ZZAPP_CONSOLE_HDR + at,
		       src + done, n);
		win_flush(ZZAPP_CONSOLE_OUT_OFF + ZZAPP_CONSOLE_HDR + at, n);
		barrier();
		win_put32(ZZAPP_CONSOLE_OUT_OFF, head + n);
		done += n;
	}
	wait_end();
	*out_n = done;
	return AAR_OK;
}

int zzapp_svc_console_read(void *buf, uint32_t len, uint32_t *out_n)
{
	uint8_t *dst = (uint8_t *)buf;

	if (!out_n || (len != 0U && !dst))
		return AAR_E_INVAL;
	*out_n = 0U;
	if (!have_window())
		return AAR_E_IO;
	zzapp_services_or_flags(ZZAPP_F_CONSOLE);
	while (len != 0U) {
		uint32_t head = win_get32(ZZAPP_CONSOLE_IN_OFF);
		uint32_t tail = win_get32(ZZAPP_CONSOLE_IN_OFF + 4U);
		uint32_t available = console_available(head, tail, ZZAPP_CONSOLE_IN_CAP);
		uint32_t at;
		uint32_t n;

		if (head - tail > ZZAPP_CONSOLE_IN_CAP) {
			wait_end();
			return AAR_E_IO;
		}
		if (available == 0U) {
			if (win_get32(ZZAPP_CONSOLE_IN_OFF + ZZAPP_CONSOLE_EOF_OFF)) {
				win_put32(ZZAPP_CONSOLE_IN_OFF + ZZAPP_CONSOLE_WANT_OFF, 0U);
				wait_end();
				return AAR_OK;
			}
			if (zzapp_services_stop_requested()) {
				win_put32(ZZAPP_CONSOLE_IN_OFF + ZZAPP_CONSOLE_WANT_OFF, 0U);
				wait_end();
				return AAR_E_IO;
			}
			/* The launcher reads the keyboard only while this is set. */
			win_put32(ZZAPP_CONSOLE_IN_OFF + ZZAPP_CONSOLE_WANT_OFF, 1U);
			wait_begin(ZZAPP_WAIT_CONSOLE_READ, head, 0U);
			yield_wait_once();
			if (wait_status() != AAR_OK) {
				wait_end();
				win_put32(ZZAPP_CONSOLE_IN_OFF + ZZAPP_CONSOLE_WANT_OFF, 0U);
				return wait_status();
			}
			continue;
		}
		win_put32(ZZAPP_CONSOLE_IN_OFF + ZZAPP_CONSOLE_WANT_OFF, 0U);
		at = tail % ZZAPP_CONSOLE_IN_CAP;
		n = ZZAPP_CONSOLE_IN_CAP - at;
		if (n > available)
			n = available;
		if (n > len)
			n = len;
		win_inval(ZZAPP_CONSOLE_IN_OFF + ZZAPP_CONSOLE_HDR + at, n);
		memcpy(dst, current_window() + ZZAPP_CONSOLE_IN_OFF + ZZAPP_CONSOLE_HDR + at, n);
		barrier();
		win_put32(ZZAPP_CONSOLE_IN_OFF + 4U, tail + n);
		wait_end();
		*out_n = n;
		return AAR_OK;
	}
	return AAR_OK;
}

int zzapp_svc_msg_send(const void *msg)
{
	uint32_t head;
	uint32_t tail;
	uint32_t slot;

	if (!msg)
		return AAR_E_INVAL;
	if (!have_window())
		return AAR_E_IO;
	head = win_get32(ZZAPP_MSG_A2H_OFF);
	tail = win_get32(ZZAPP_MSG_A2H_OFF + 4U);
	if (head - tail > ZZAPP_MSG_CAP)
		return AAR_E_IO;
	if (head - tail == ZZAPP_MSG_CAP)
		return AAR_E_BUSY;
	slot = head & (ZZAPP_MSG_CAP - 1U);
	memcpy(current_window() + ZZAPP_MSG_A2H_OFF + ZZAPP_MSG_HDR +
	       slot * ZZAPP_MSG_BYTES_PER, msg, ZZAPP_MSG_BYTES_PER);
	win_flush(ZZAPP_MSG_A2H_OFF + ZZAPP_MSG_HDR +
		  slot * ZZAPP_MSG_BYTES_PER, ZZAPP_MSG_BYTES_PER);
	barrier();
	win_put32(ZZAPP_MSG_A2H_OFF, head + 1U);
	return AAR_OK;
}

int zzapp_svc_msg_poll(void *msg)
{
	uint32_t head;
	uint32_t tail;
	uint32_t slot;

	if (!msg)
		return AAR_E_INVAL;
	if (!have_window())
		return AAR_E_IO;
	head = win_get32(ZZAPP_MSG_H2A_OFF);
	tail = win_get32(ZZAPP_MSG_H2A_OFF + 4U);
	if (head - tail > ZZAPP_MSG_CAP)
		return AAR_E_IO;
	if (head == tail)
		return 0;
	slot = tail & (ZZAPP_MSG_CAP - 1U);
	win_inval(ZZAPP_MSG_H2A_OFF + ZZAPP_MSG_HDR +
		  slot * ZZAPP_MSG_BYTES_PER, ZZAPP_MSG_BYTES_PER);
	memcpy(msg, current_window() + ZZAPP_MSG_H2A_OFF + ZZAPP_MSG_HDR +
	       slot * ZZAPP_MSG_BYTES_PER, ZZAPP_MSG_BYTES_PER);
	barrier();
	win_put32(ZZAPP_MSG_H2A_OFF + 4U, tail + 1U);
	return 1;
}

/*
 * A queued message ends the wait before anything else, so a stop never
 * drops one. The wait is level-triggered: it re-reads the ring after every
 * resume, and the service pass wakes any parked EVENT waiter whose ring is
 * not empty, so a push between the check and the park cannot be missed.
 */
int zzapp_svc_msg_wait(uint32_t timeout_us)
{
	uint64_t deadline = 0U;
	int resume_status = AAR_OK;

	if (!have_window())
		return AAR_E_IO;
	if (timeout_us != 0U)
		deadline = now_us() + timeout_us;
	for (;;) {
		uint32_t head = win_get32(ZZAPP_MSG_H2A_OFF);
		uint32_t tail = win_get32(ZZAPP_MSG_H2A_OFF + 4U);

		if (head - tail > ZZAPP_MSG_CAP) {
			wait_end();
			return AAR_E_IO;
		}
		if (head != tail) {
			wait_end();
			return AAR_OK;
		}
		/* The ring is read before the wake reason is honoured, so a
		 * message that arrived while the wake said timeout or cancel
		 * is still returned. */
		if (resume_status != AAR_OK) {
			wait_end();
			return resume_status;
		}
		if (timeout_us == 0U)
			return AAR_E_TIMEDOUT;
		if (zzapp_services_stop_requested()) {
			wait_end();
			return AAR_E_IO;
		}
		if (now_us() >= deadline) {
			wait_end();
			return AAR_E_TIMEDOUT;
		}
		wait_begin(ZZAPP_WAIT_EVENT, tail, deadline);
		yield_wait_once();
		resume_status = wait_status();
	}
}

static int span_ok(uint32_t addr, uint32_t bytes)
{
	uint64_t end;

	if (addr == 0U || bytes == 0U)
		return 0;
	end = (uint64_t)addr + bytes;
	if (addr >= ZZAPP_P96_LO && end <= ZZAPP_P96_HI)
		return 1;
	if (addr >= ZZAPP_HEAP_LO && end <= ZZAPP_HEAP_HI)
		return 1;
	return 0;
}

static uint32_t format_bpp(uint32_t format)
{
	if (format == AAR_FMT_RGB565)
		return 2U;
	if (format == AAR_FMT_BGRA8888)
		return 4U;
	if (format == AAR_FMT_CLUT8)
		return 1U;
	return 0U;
}

static uint32_t default_layout(uint32_t format)
{
	if (format == AAR_FMT_RGB565)
		return AAR_LAYOUT_RGB565_BE;
	if (format == AAR_FMT_BGRA8888)
		return AAR_LAYOUT_BGRA8888;
	if (format == AAR_FMT_CLUT8)
		return AAR_LAYOUT_CLUT8;
	return 0U;
}

static int layout_matches_format(uint32_t format, uint32_t layout)
{
	if (layout == 0U)
		layout = default_layout(format);
	if (format == AAR_FMT_RGB565)
		return layout == AAR_LAYOUT_RGB565_BE ||
		       layout == AAR_LAYOUT_RGB565_LE ||
		       layout == AAR_LAYOUT_BGR565_BE ||
		       layout == AAR_LAYOUT_BGR565_LE;
	if (format == AAR_FMT_BGRA8888)
		return layout == AAR_LAYOUT_BGRA8888 ||
		       layout == AAR_LAYOUT_RGBA8888;
	if (format == AAR_FMT_CLUT8)
		return layout == AAR_LAYOUT_CLUT8;
	return 0;
}

static void drop_owned_back(void)
{
	uint32_t back;

	if (!current_runtime()->display_back_owned)
		return;
	back = current_runtime()->display_back;
	current_runtime()->display_back_owned = 0;
	if (back != 0U)
		zzapp_loader_free_back(back);
}

static void display_reap_flip(void)
{
	uint32_t ack;

	if (!current_runtime()->display_bound)
		return;
	if (display_mailbox_enabled())
		return;
	ack = life_get(ZZAPP_LIFE_FLIP_ACK_OFF);
	if (ack == current_runtime()->display_ack_seen)
		return;
	current_runtime()->display_ack_seen = ack;
	if (current_runtime()->display_queued != 0U) {
		uint32_t front = current_runtime()->display_front;

		current_runtime()->display_front = current_runtime()->display_queued;
		current_runtime()->display_spare = front;
		life_put(ZZAPP_LIFE_CARD_FRONT_OFF,
			 current_runtime()->display_front);
		life_put(ZZAPP_LIFE_CARD_SPARE_OFF,
			 current_runtime()->display_spare);
		current_runtime()->display_queued = 0U;
	} else if (!current_runtime()->display_back_owned &&
		   current_runtime()->display_back != 0U) {
		uint32_t front = current_runtime()->display_front;

		current_runtime()->display_front = current_runtime()->display_back;
		current_runtime()->display_back = front;
		life_put(ZZAPP_LIFE_CARD_FRONT_OFF,
			 current_runtime()->display_front);
		life_put(ZZAPP_LIFE_CARD_BACK_OFF,
			 current_runtime()->display_back);
	}
}

int zzapp_display_flip_idle(void)
{
	if (!current_runtime()->display_bound)
		return 0;
	if (display_mailbox_enabled())
		return 1;
	/* One window read per poll: reap reads the ack, and the generation
	 * counter is core-1-private state cached beside it. */
	display_reap_flip();
	return current_runtime()->display_flip_gen ==
	       current_runtime()->display_ack_seen;
}

/*
 * Core 0 calls begin/end around init_vdma from the vblank ISR.  The lock is
 * deliberately held across that programming step: either core 1 replaces the
 * pending buffer before selection, or it waits until the selected address has
 * become VDMA's next frame store.  There is no ownership interleaving.
 */
int zzapp_services_display_mailbox_begin(uint32_t normal_scanout,
					 uint32_t *scanout)
{
	uint32_t slot;

	if (!scanout)
		return 0;
	for (slot = 0U; slot < ZZAPP_SLOT_COUNT; slot++) {
		struct zzapp_service_runtime *state = &runtime[slot];

		if (!__atomic_load_n(&state->display_mailbox_active,
				     __ATOMIC_ACQUIRE) ||
		    !display_mailbox_owns_state(state, normal_scanout))
			continue;
		display_mailbox_lock_acquire_for(state);
		if (!state->display_mailbox_active ||
		    !display_mailbox_owns_state(state, normal_scanout)) {
			display_mailbox_lock_release_for(state);
			continue;
		}
		*scanout = state->display_mailbox_pending != 0U ?
			state->display_mailbox_pending : state->display_mailbox_shown;
		state->display_mailbox_latch_pending =
			state->display_mailbox_pending != 0U;
		display_mailbox_slot = (int)slot;
		return 1;
	}
	return 0;
}

void zzapp_services_display_mailbox_end(int latched)
{
	uint32_t slot;
	struct zzapp_service_runtime *state;

	if (display_mailbox_slot < 0 ||
	    display_mailbox_slot >= (int)ZZAPP_SLOT_COUNT)
		return;
	slot = (uint32_t)display_mailbox_slot;
	state = &runtime[slot];
	if (latched && state->display_mailbox_latch_pending) {
		uint32_t old_shown = state->display_mailbox_shown;

		state->display_mailbox_shown = state->display_mailbox_pending;
		state->display_mailbox_pending = 0U;
		state->display_front = state->display_mailbox_shown;
		state->display_spare = old_shown;
		life_put_slot(slot, ZZAPP_LIFE_CARD_FRONT_OFF,
			      state->display_front);
		life_put_slot(slot, ZZAPP_LIFE_CARD_SPARE_OFF,
			      state->display_spare);
	}
	state->display_mailbox_latch_pending = 0;
	display_mailbox_slot = -1;
	display_mailbox_lock_release_for(state);
}

uint32_t zzapp_svc_display_latest_dropped(void)
{
	if (!display_mailbox_enabled())
		return 0U;
	return life_get(ZZAPP_LIFE_MAILBOX_REPLACED_OFF);
}

void zzapp_services_display_vblank(uint32_t scanout_address)
{
	uint32_t slot;
	uint32_t selected = selected_slot[service_context()];

	for (slot = 0U; slot < ZZAPP_SLOT_COUNT; slot++) {
		uint32_t flags;
		uint32_t gen;

		select_slot(slot);
		if (!current_runtime()->display_bound || display_mailbox_enabled())
			continue;
		flags = life_get(ZZAPP_LIFE_FLAGS_OFF);
		if ((flags & (ZZAPP_F_LAUNCHER_SCANOUT_ACK | ZZAPP_F_SCANOUT_ACK)) !=
		    (ZZAPP_F_LAUNCHER_SCANOUT_ACK | ZZAPP_F_SCANOUT_ACK))
			continue;
		gen = life_get(ZZAPP_LIFE_FLIP_GEN_OFF);
		if (gen == life_get(ZZAPP_LIFE_FLIP_ACK_OFF) ||
		    gen != life_get(ZZAPP_LIFE_FLIP_SUBMIT_OFF) ||
		    scanout_address != life_get(ZZAPP_LIFE_FLIP_ADDRESS_OFF))
			continue;
		life_put(ZZAPP_LIFE_FLIP_ACK_OFF, gen);
	}
	select_slot(selected);
}

int zzapp_display_bound(uint32_t *w, uint32_t *h, uint32_t *pitch,
			uint32_t *format, uint32_t *card_front,
			uint32_t *card_back)
{
	struct zzapp_service_runtime *state = current_runtime();

	if (!state->display_bound)
		return AAR_E_INVAL;
	display_reap_flip();
	if (w)
		*w = state->display_width;
	if (h)
		*h = state->display_height;
	if (pitch)
		*pitch = state->display_pitch;
	if (format)
		*format = state->display_format;
	if (card_front)
		*card_front = state->display_front;
	if (card_back)
		*card_back = state->display_back;
	return AAR_OK;
}

int zzapp_display_layout(uint32_t *layout)
{
	if (!layout || !current_runtime()->display_bound)
		return AAR_E_INVAL;
	*layout = current_runtime()->display_layout;
	return AAR_OK;
}

static uint32_t display_alias(uint32_t physical)
{
	struct zzapp_service_runtime *state = current_runtime();
	uint32_t i;

	for (i = 0U; i < 3U; i++) {
		if (state->display_alias_phys[i] == physical)
			return state->display_alias[i];
	}
	return 0U;
}

static int fill_surface(aar_surface *out)
{
	uint32_t w;
	uint32_t h;
	uint32_t pitch;
	uint32_t format;
	uint32_t front;
	uint32_t back;
	uint32_t front_alias;
	uint32_t back_alias;
	uint32_t caps = AAR_DISPCAP_ASYNC;

	if (!out || !current_runtime()->display_bound)
		return AAR_E_INVAL;
	if (zzapp_display_bound(&w, &h, &pitch, &format, &front, &back) != AAR_OK)
		return AAR_E_INVAL;
	front_alias = display_alias(front);
	back_alias = display_alias(back);
	if (display_mailbox_enabled())
		caps |= AAR_DISPCAP_LATEST;
	if (life_get(ZZAPP_LIFE_FLAGS_OFF) & ZZAPP_F_LAUNCHER_CLUT8)
		caps |= AAR_DISPCAP_CLUT8;
	out->id = current_runtime()->display_id;
	out->width = w;
	out->height = h;
	out->pitch = pitch;
	out->format = format;
	out->layout = current_runtime()->display_layout;
	out->caps = caps;
	out->reserved = 0U;
	out->front = (void *)(uintptr_t)front_alias;
	out->back = (void *)(uintptr_t)back_alias;
	return front_alias != 0U && back_alias != 0U ? AAR_OK : AAR_E_INVAL;
}

int zzapp_display_flip_bound(void)
{
	struct zzapp_service_runtime *state = current_runtime();

	if (!state->display_bound)
		return AAR_E_INVAL;
	display_reap_flip();
	if (state->display_flip_gen != state->display_ack_seen)
		return AAR_E_BUSY;
	state->display_flip_gen++;
	life_put(ZZAPP_LIFE_FLIP_GEN_OFF, state->display_flip_gen);
	return AAR_OK;
}

/* A flip normally completes within a few VBlanks; one line per wait that
 * outlasts this says which half of the handshake is late. */
#define FLIP_WAIT_REPORT_US 50000U

#ifdef ZZAPP_HOST_TEST
static uint32_t flip_wait_reports;
#endif

/* gen != submit: ZZRun has not shown the frame yet.  submit == gen but no
 * ack: the VBlank has not seen the flip address on scanout. */
static void report_slow_flip(uint32_t gen, uint64_t waited_us)
{
#ifndef ZZAPP_HOST_TEST
	printf("[app] flip wait %lu ms: gen=%lu ack=%lu submit=%lu addr=%08lx flags=%08lx queued=%08lx\r\n",
	       (unsigned long)(waited_us / 1000U), (unsigned long)gen,
	       (unsigned long)life_get(ZZAPP_LIFE_FLIP_ACK_OFF),
	       (unsigned long)life_get(ZZAPP_LIFE_FLIP_SUBMIT_OFF),
	       (unsigned long)life_get(ZZAPP_LIFE_FLIP_ADDRESS_OFF),
	       (unsigned long)life_get(ZZAPP_LIFE_FLAGS_OFF),
	       (unsigned long)current_runtime()->display_queued);
#else
	(void)gen;
	(void)waited_us;
	flip_wait_reports++;
#endif
}

/* A short bounded poll before parking: when the VBlank ack is already on
 * the life page - the common case for an app that presents triple-buffered
 * frames - the wait completes with no scheduler round trip at all. */
#define FLIP_POLL_ACKS 8U

static int wait_flip(uint32_t gen)
{
    uint32_t generation = zzapp_services_generation();
    uint64_t started = now_us();
    int reported = 0;
    uint32_t poll;

    for (poll = 0U; poll < FLIP_POLL_ACKS; poll++) {
        if (zzapp_services_stop_requested() ||
            zzapp_services_generation() != generation)
            return AAR_E_IO;
        if (life_get(ZZAPP_LIFE_FLIP_ACK_OFF) == gen) {
            display_reap_flip();
            return AAR_OK;
        }
    }
    for (;;) {
        uint32_t ack;
        int status;

        if (zzapp_services_stop_requested() ||
            zzapp_services_generation() != generation)
            return AAR_E_IO;
        ack = life_get(ZZAPP_LIFE_FLIP_ACK_OFF);
        if (ack == gen) {
            display_reap_flip();
            wait_end();
            return AAR_OK;
        }
        /* The first registration wakes this parked app at 50 ms solely for
         * diagnostics. Subsequent waiting remains event-driven until ack. */
        wait_begin(ZZAPP_WAIT_FLIP, gen,
                   reported ? 0U : started + FLIP_WAIT_REPORT_US);
        yield_wait_once();
        status = wait_status();
        if (status == AAR_E_TIMEDOUT && !reported) {
            reported = 1;
            wait_end();
            report_slow_flip(gen, now_us() - started);
            continue;
        }
        if (status != AAR_OK) {
            wait_end();
            return status;
        }
    }
}

int zzapp_display_wait_flip_idle(void)
{
	if (!current_runtime()->display_bound)
		return AAR_E_INVAL;
	if (display_mailbox_enabled())
		return AAR_OK;
	return wait_flip(current_runtime()->display_flip_gen);
}


int zzapp_svc_display_open(uint32_t w, uint32_t h, uint32_t format,
			   uint32_t flags, aar_surface *out)
{
	uint32_t bpp;
	uint32_t bits;

	if (!out || !have_window())
		return AAR_E_INVAL;
	if ((flags & ~(AAR_DISPLAY_VSYNC | AAR_DISPLAY_LATEST)) != 0U)
		return AAR_E_INVAL;
	bpp = format_bpp(format);
	if (w == 0U || h == 0U || w > AAR_DISPLAY_MAX_DIM ||
	    h > AAR_DISPLAY_MAX_DIM || bpp == 0U)
		return AAR_E_INVAL;
	if (format == AAR_FMT_CLUT8 &&
	    (life_get(ZZAPP_LIFE_FLAGS_OFF) & ZZAPP_F_LAUNCHER_CLUT8) == 0U)
		return AAR_E_NOSYS;
	if (current_runtime()->display_bound)
		return AAR_E_BUSY;
	bits = life_get(ZZAPP_LIFE_FLAGS_OFF);
	bits |= ZZAPP_F_DISPLAY_UNBOUND;
	bits &= ~(ZZAPP_F_VSYNC | ZZAPP_F_MAILBOX_REQUEST);
	if ((flags & AAR_DISPLAY_VSYNC) != 0U)
		bits |= ZZAPP_F_VSYNC;
	if ((flags & AAR_DISPLAY_LATEST) != 0U)
		bits |= ZZAPP_F_MAILBOX_REQUEST;
	life_put(ZZAPP_LIFE_WIDTH_OFF, w);
	life_put(ZZAPP_LIFE_HEIGHT_OFF, h);
	life_put(ZZAPP_LIFE_FORMAT_OFF, format);
	life_put(ZZAPP_LIFE_FLAGS_OFF, bits);
	for (;;) {
		if (zzapp_services_stop_requested())
			return AAR_E_IO;
		if ((life_get(ZZAPP_LIFE_FLAGS_OFF) & ZZAPP_F_DISPLAY_UNBOUND) == 0U &&
		    current_runtime()->display_bound)
			break;
		wait_begin(ZZAPP_WAIT_DISPLAY_OPEN, 0U, 0U);
		yield_wait_once();
		if (wait_status() != AAR_OK) {
			wait_end();
			return wait_status();
		}
	}
	wait_end();
	return fill_surface(out);
}

static void record_present_flush(unsigned long card, unsigned long bytes)
{
#ifdef ZZAPP_HOST_TEST
	if (zzapp_test_present_flush_count < 32U) {
		zzapp_test_present_flushes[zzapp_test_present_flush_count] =
			(struct zzapp_present_flush_record){
				.card = card, .bytes = bytes
			};
		zzapp_test_present_flush_count++;
	}
#endif
}

static void flush_presented(uint32_t addr, uint32_t bytes)
{
	if (addr != 0U && bytes != 0U) {
		record_present_flush(addr, bytes);
#ifndef ZZAPP_HOST_TEST
		/*
		 * Clean the frame through L1 and L2 to DDR before queuing the
		 * flip.  The HP scanout port observes DDR, so invalidation is
		 * unnecessary; retaining the lines avoids the next frame's
		 * write-allocate misses.  The L1 data cache is PIPT, so this
		 * clean by the card VA also reaches lines dirtied through the
		 * app's private alias.  Core 0's vblank way flush is not enough:
		 * scanout can select this buffer before it reaches its lines.
		 * l2_maint serializes with core 0 in 4 KiB chunks.
		 */
		l2_maint_dcache_clean_range((intptr_t)addr, bytes);
#endif
	}
	barrier();
}

static int display_mailbox_flip(uint32_t back)
{
	uint32_t spare;
	uint32_t gen;

	if (back == 0U)
		return AAR_E_INVAL;
	display_mailbox_lock_acquire();
	if (!current_runtime()->display_mailbox_active) {
		display_mailbox_lock_release();
		return AAR_E_BUSY;
	}
	if (current_runtime()->display_mailbox_pending != 0U) {
		current_runtime()->display_back =
			current_runtime()->display_mailbox_pending;
		life_put(ZZAPP_LIFE_CARD_BACK_OFF,
			 current_runtime()->display_back);
		current_runtime()->display_mailbox_pending = back;
		life_put(ZZAPP_LIFE_MAILBOX_REPLACED_OFF,
			 life_get(ZZAPP_LIFE_MAILBOX_REPLACED_OFF) + 1U);
	} else {
		spare = current_runtime()->display_spare;
		if (spare == 0U) {
			display_mailbox_lock_release();
			return AAR_E_BUSY;
		}
		current_runtime()->display_mailbox_pending = back;
		current_runtime()->display_back = spare;
		current_runtime()->display_spare = 0U;
		life_put(ZZAPP_LIFE_CARD_BACK_OFF, spare);
		life_put(ZZAPP_LIFE_CARD_SPARE_OFF, 0U);
	}
	/* Keep generic flip clients idle without exposing a VBlank wait. */
	gen = current_runtime()->display_flip_gen + 1U;
	life_put(ZZAPP_LIFE_FLIP_GEN_OFF, gen);
	life_put(ZZAPP_LIFE_FLIP_ACK_OFF, gen);
	current_runtime()->display_flip_gen = gen;
	current_runtime()->display_ack_seen = gen;
	display_mailbox_lock_release();
	return AAR_OK;
}

int zzapp_svc_display_set_palette(uint32_t surface_id, uint32_t first,
				  uint32_t count, const uint32_t *rgb)
{
	uint32_t i;
	uint32_t gen;

	if (!current_runtime()->display_bound || surface_id != current_runtime()->display_id ||
	    life_get(ZZAPP_LIFE_FORMAT_OFF) != AAR_FMT_CLUT8)
		return AAR_E_INVAL;
	if (!rgb || count == 0U || first >= 256U || count > 256U - first)
		return AAR_E_INVAL;
	for (i = 0U; i < count; i++)
		win_put32(ZZAPP_PALETTE_OFF + (first + i) * sizeof(uint32_t),
			  rgb[i] & 0x00ffffffU);
	barrier();
	life_put(ZZAPP_LIFE_PALETTE_FIRST_OFF, first);
	life_put(ZZAPP_LIFE_PALETTE_COUNT_OFF, count);
	gen = life_get(ZZAPP_LIFE_PALETTE_GEN_OFF) + 1U;
	life_put(ZZAPP_LIFE_PALETTE_GEN_OFF, gen);
	return AAR_OK;
}

int zzapp_svc_display_flip_async(uint32_t surface_id)
{
	struct zzapp_service_runtime *state = current_runtime();
	uint32_t front;
	uint32_t back;
	uint32_t present;
	uint32_t flags;
	int rc;

	if (!state->display_bound || surface_id != state->display_id)
		return AAR_E_INVAL;
	if (!zzapp_display_flip_idle())
		return AAR_E_BUSY;
	front = state->display_front;
	back = state->display_back;
	if (display_mailbox_enabled()) {
		flush_presented(back, state->display_bytes);
		return display_mailbox_flip(back);
	}
	if (state->display_back_owned) {
		if (front == 0U || back == 0U)
			return AAR_E_INVAL;
		memcpy((void *)(uintptr_t)front, (const void *)(uintptr_t)back,
		       state->display_bytes);
		flush_presented(front, state->display_bytes);
		present = front;
	} else if (state->display_spare != 0U) {
		flush_presented(back, state->display_bytes);
		present = back;
		state->display_queued = back;
		state->display_back = state->display_spare;
		state->display_spare = 0U;
		life_put(ZZAPP_LIFE_CARD_BACK_OFF, state->display_back);
		life_put(ZZAPP_LIFE_CARD_SPARE_OFF, 0U);
	} else {
		present = back != 0U ? back : front;
		flush_presented(present, state->display_bytes);
	}
	state->display_present = present;
	flags = life_get(ZZAPP_LIFE_FLAGS_OFF);
	if ((flags & (ZZAPP_F_LAUNCHER_SCANOUT_ACK | ZZAPP_F_SCANOUT_ACK)) ==
	    (ZZAPP_F_LAUNCHER_SCANOUT_ACK | ZZAPP_F_SCANOUT_ACK)) {
		life_put(ZZAPP_LIFE_FLIP_ADDRESS_OFF, present);
		life_put(ZZAPP_LIFE_FLIP_SUBMIT_OFF, 0U);
	}
	rc = zzapp_display_flip_bound();
	return rc;
}

int zzapp_svc_display_flip(uint32_t surface_id)
{
	uint32_t gen;
	int rc;

	rc = zzapp_svc_display_flip_async(surface_id);
	if (rc != AAR_OK)
		return rc;
	gen = current_runtime()->display_flip_gen;
	return wait_flip(gen);
}

int zzapp_svc_display_refresh(aar_surface *out)
{
	if (!out || out->id != current_runtime()->display_id)
		return AAR_E_INVAL;
	if (!zzapp_display_flip_idle() && current_runtime()->display_queued == 0U)
		return AAR_E_BUSY;
	return fill_surface(out);
}

int zzapp_svc_display_vsync(void)
{
	if (!current_runtime()->display_bound)
		return AAR_E_INVAL;
	zzapp_services_or_flags(ZZAPP_F_VSYNC);
	yield_once();
	return AAR_OK;
}

int zzapp_svc_display_close(uint32_t surface_id)
{
	if (!current_runtime()->display_bound || surface_id != current_runtime()->display_id)
		return AAR_E_INVAL;
	revoke_surface_grants();
	zzapp_kernel_surface_unbind(zzapp_runtime_kernel(), service_slot());
	drop_owned_back();
	current_runtime()->display_bound = 0;
	display_mailbox_disable();
	current_runtime()->display_id = 0U;
	zzapp_services_and_flags(~(ZZAPP_F_DISPLAY_UNBOUND | ZZAPP_F_VSYNC |
				   ZZAPP_F_MAILBOX_REQUEST | ZZAPP_F_MAILBOX));
	life_put(ZZAPP_LIFE_CARD_FRONT_OFF, 0U);
	life_put(ZZAPP_LIFE_CARD_BACK_OFF, 0U);
	life_put(ZZAPP_LIFE_CARD_SPARE_OFF, 0U);
	return AAR_OK;
}

static int slot_is_busy(uint32_t slot)
{
	if (slot < 4U && slot_busy_test[slot])
		return 1;
#ifndef ZZAPP_HOST_TEST
	{
		struct audio_fabric_slot *s = fabric_slot(slot);

		if (s != NULL && s->attached)
			return 1;
	}
#endif
	return 0;
}

/*
 * The app's public write cursor is a 32-bit frame index: it wraps after
 * ~54 h at 22050 Hz, while audio_staged is a monotonic 64-bit byte count.
 * Widening the raw counter makes produced < staged at the wrap and the
 * producer stalls. The app is never more than one ring ahead of what was
 * staged, so extend the cursor from the staged frame count by the modular
 * difference. No state: the fabric's snapshot and stage paths can both call
 * this. (The AHI pump already compares modular uint32 differences.)
 */
__attribute__((unused))
static uint64_t audio_produced_frames(uint32_t write_frame, uint64_t staged_bytes)
{
	uint64_t staged_frames = staged_bytes / 4U;

	return staged_frames + (uint32_t)(write_frame - (uint32_t)staged_frames);
}

#ifndef ZZAPP_HOST_TEST
static struct zzapp_service_runtime *audio_runtime(uint32_t producer_slot)
{
	uint32_t slot;

	for (slot = 0U; slot < ZZAPP_SLOT_COUNT; slot++) {
		struct zzapp_service_runtime *state = &runtime[slot];

		if (state->audio_open && !state->audio_ahi_route &&
		    state->audio_slot == producer_slot)
			return state;
	}
	return NULL;
}

static int app_audio_snapshot_for(uint32_t producer_slot,
				  struct audio_fabric_source *source)
{
	struct zzapp_service_runtime *state = audio_runtime(producer_slot);
	uint32_t wr;
	uint64_t staged;

	if (!state || !state->audio_samples || !state->audio_write_frame)
		return 0;
	wr = *state->audio_write_frame;
	staged = state->audio_staged;
	memset(source, 0, sizeof(*source));
	source->ring = (uint8_t *)state->audio_samples;
	source->capacity = ZZAPP_AUDIO_FRAMES * 4U;
	source->produced_bytes = audio_produced_frames(wr, staged) * 4U;
	source->staged_bytes = staged;
	source->sample_rate = ZZAPP_AUDIO_RATE;
	source->channels = 2U;
	source->sample_format = 1U; /* SDK_AUDIO_SAMPLE_FORMAT_S16LE */
#if ZZAPP_PERF_COUNTERS
	if (source->produced_bytes - source->staged_bytes <= source->capacity / 4U) {
		if (!state->audio_low_water_latched) {
			zzapp_kernel_perf_audio(zzapp_runtime_kernel(),
						(uint32_t)(state - runtime), 1U, 0U,
						0U);
			state->audio_low_water_latched = 1U;
		}
	} else {
		state->audio_low_water_latched = 0U;
	}
#endif
	return 1;
}

static int app_audio_stage_for(uint32_t producer_slot, uint32_t bytes)
{
	struct zzapp_service_runtime *state = audio_runtime(producer_slot);
	uint64_t produced;

	if (!state || !state->audio_write_frame)
		return 0;
	produced = audio_produced_frames(*state->audio_write_frame,
					 state->audio_staged) * 4U;
	if (produced < state->audio_staged ||
	    produced - state->audio_staged < bytes)
		return 0;
	state->audio_staged += bytes;
	return 1;
}

static void app_audio_retire_for(uint32_t producer_slot, uint32_t bytes)
{
	struct zzapp_service_runtime *state = audio_runtime(producer_slot);

	if (!state || !state->audio_read_frame || (bytes & 3U) != 0U)
		return;
	*state->audio_read_frame += bytes / 4U;
}

static int app_audio_snapshot_mailbox(struct audio_fabric_source *source)
{
	return app_audio_snapshot_for(AUDIO_FABRIC_SLOT_MAILBOX, source);
}

static int app_audio_stage_mailbox(uint32_t bytes)
{
	return app_audio_stage_for(AUDIO_FABRIC_SLOT_MAILBOX, bytes);
}

static void app_audio_retire_mailbox(uint32_t bytes)
{
	app_audio_retire_for(AUDIO_FABRIC_SLOT_MAILBOX, bytes);
}

static int app_audio_snapshot_reserved(struct audio_fabric_source *source)
{
	return app_audio_snapshot_for(AUDIO_FABRIC_SLOT_RESERVED, source);
}

static int app_audio_stage_reserved(uint32_t bytes)
{
	return app_audio_stage_for(AUDIO_FABRIC_SLOT_RESERVED, bytes);
}

static void app_audio_retire_reserved(uint32_t bytes)
{
	app_audio_retire_for(AUDIO_FABRIC_SLOT_RESERVED, bytes);
}

#if ZZAPP_PERF_COUNTERS
void audio_fabric_starvation_event(uint32_t producer_slot)
{
	struct zzapp_service_runtime *state = audio_runtime(producer_slot);

	if (state)
		zzapp_kernel_perf_audio(zzapp_runtime_kernel(),
					(uint32_t)(state - runtime), 0U, 1U, 0U);
}
#endif

static void app_audio_underrun_for(uint32_t producer_slot)
{
#if ZZAPP_PERF_COUNTERS
	struct zzapp_service_runtime *state = audio_runtime(producer_slot);

	if (state)
		zzapp_kernel_perf_audio(zzapp_runtime_kernel(),
					(uint32_t)(state - runtime), 0U, 0U, 1U);
#else
	(void)producer_slot;
#endif
}

static void app_audio_underrun_mailbox(void)
{
	app_audio_underrun_for(AUDIO_FABRIC_SLOT_MAILBOX);
}

static void app_audio_underrun_reserved(void)
{
	app_audio_underrun_for(AUDIO_FABRIC_SLOT_RESERVED);
}

static void app_audio_nop(void)
{
}

static const struct audio_fabric_producer_ops app_audio_mailbox_ops = {
	.snapshot = app_audio_snapshot_mailbox,
	.stage = app_audio_stage_mailbox,
	.retire = app_audio_retire_mailbox,
	.underrun = app_audio_underrun_mailbox,
	.gone = app_audio_nop,
	.tail_real = app_audio_nop,
	.tail_drained = app_audio_nop
};

static const struct audio_fabric_producer_ops app_audio_reserved_ops = {
	.snapshot = app_audio_snapshot_reserved,
	.stage = app_audio_stage_reserved,
	.retire = app_audio_retire_reserved,
	.underrun = app_audio_underrun_reserved,
	.gone = app_audio_nop,
	.tail_real = app_audio_nop,
	.tail_drained = app_audio_nop
};

static const struct audio_fabric_producer_ops *app_audio_ops_for_slot(
	uint32_t producer_slot)
{
	return producer_slot == AUDIO_FABRIC_SLOT_MAILBOX ?
		&app_audio_mailbox_ops : &app_audio_reserved_ops;
}
#endif

static void audio_pump_ahi_window(void)
{
	uint32_t host_head;
	uint32_t host_tail;
	uint32_t source_read;
	uint32_t source_staged;
	uint32_t source_write;
	uint32_t retired;
	uint32_t available;
	uint32_t free_bytes;
	uint32_t copied = 0U;

	if (!current_runtime()->audio_open || !current_runtime()->audio_ahi_route || !have_window() ||
	    !current_runtime()->audio_samples || !current_runtime()->audio_read_frame || !current_runtime()->audio_write_frame)
		return;
	host_head = win_get32(ZZAPP_AUDIO_OFF + ZZAPP_AUDIO_HEAD_OFF);
	host_tail = win_get32(ZZAPP_AUDIO_OFF + ZZAPP_AUDIO_TAIL_OFF);
	retired = host_tail - current_runtime()->audio_host_tail_seen;
	if (host_head - host_tail > ZZAPP_AUDIO_CAP ||
	    retired > ZZAPP_AUDIO_CAP || (retired & 3U) != 0U)
		return;
	source_read = *current_runtime()->audio_read_frame;
	source_staged = current_runtime()->audio_ahi_staged_frame;
	if (source_staged - source_read > ZZAPP_AUDIO_FRAMES ||
	    retired / 4U > source_staged - source_read)
		return;
	if (retired != 0U) {
		*current_runtime()->audio_read_frame = source_read + retired / 4U;
		current_runtime()->audio_host_tail_seen = host_tail;
		source_read = *current_runtime()->audio_read_frame;
	}
	source_staged = current_runtime()->audio_ahi_staged_frame;
	source_write = *current_runtime()->audio_write_frame;
	if (source_write - source_staged > ZZAPP_AUDIO_FRAMES)
		return;
	available = (source_write - source_staged) * 4U;
	free_bytes = ZZAPP_AUDIO_CAP - (host_head - host_tail);
#if ZZAPP_PERF_COUNTERS
	if (available <= ZZAPP_AUDIO_CAP / 4U) {
		if (!current_runtime()->audio_low_water_latched) {
			zzapp_kernel_perf_audio(zzapp_runtime_kernel(), service_slot(),
						1U, 0U, 0U);
			current_runtime()->audio_low_water_latched = 1U;
		}
	} else {
		current_runtime()->audio_low_water_latched = 0U;
	}
#endif
	while (copied < available && copied < free_bytes) {
		uint32_t src_at = (source_staged * 4U + copied) %
			(ZZAPP_AUDIO_FRAMES * 4U);
		uint32_t dst_at = (host_head + copied) % ZZAPP_AUDIO_CAP;
		uint32_t n = available - copied;
		uint32_t src_left = ZZAPP_AUDIO_FRAMES * 4U - src_at;
		uint32_t dst_left = ZZAPP_AUDIO_CAP - dst_at;

		if (n > free_bytes - copied)
			n = free_bytes - copied;
		if (n > src_left)
			n = src_left;
		if (n > dst_left)
			n = dst_left;
		memcpy(current_window() + ZZAPP_AUDIO_OFF + ZZAPP_AUDIO_HDR + dst_at,
		       (const uint8_t *)current_runtime()->audio_samples + src_at, n);
		win_flush(ZZAPP_AUDIO_OFF + ZZAPP_AUDIO_HDR + dst_at, n);
		copied += n;
	}
	if (copied != 0U) {
		current_runtime()->audio_ahi_staged_frame += copied / 4U;
		barrier();
		win_put32(ZZAPP_AUDIO_OFF + ZZAPP_AUDIO_HEAD_OFF,
			  host_head + copied);
	}
}
static void revoke_audio_grant(void)
{
	struct zzapp_service_runtime *state = current_runtime();
	zzapp_proc_t *proc = zzapp_proc_slot(service_slot());

	if (proc && state->audio_grant_bytes != 0U)
		(void)zzapp_as_alias_unmap(&proc->as, state->audio_grant_base,
					    state->audio_grant_bytes);
	state->audio_grant_base = 0U;
	state->audio_grant_bytes = 0U;
	state->audio_alias = 0U;
}


static void audio_detach(void)
{
	int open = 1;
	int was_ahi_route;

	/* Core 0 releases a close only after core 1 has quiesced the AHI pump. */
	if (!__atomic_compare_exchange_n(&current_runtime()->audio_open, &open, 0, 0,
					 __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
		return;
#ifdef ZZAPP_HOST_TEST
	current_runtime()->audio_detach_count++;
#endif
	was_ahi_route = current_runtime()->audio_ahi_route;
#ifdef ZZAPP_HOST_TEST
	(void)was_ahi_route;
#endif
	revoke_audio_grant();
#ifndef ZZAPP_HOST_TEST
	if (!was_ahi_route) {
		audio_fabric_producer_detach(current_runtime()->audio_slot);
	}
	if (current_runtime()->audio_block)
	{
		void *block = current_runtime()->audio_block;

		/* Detached before the free, see forget_window. */
		current_runtime()->audio_block = NULL;
		if (block)
			surface_allocator_free((uint32_t)(uintptr_t)block);
	}
#endif
#ifdef ZZAPP_HOST_TEST
	free(current_runtime()->audio_block);
#endif
	current_runtime()->audio_block = NULL;
	current_runtime()->audio_samples = NULL;
	current_runtime()->audio_read_frame = NULL;
	current_runtime()->audio_write_frame = NULL;
	current_runtime()->audio_staged = 0U;
	current_runtime()->audio_ahi_staged_frame = 0U;
	current_runtime()->audio_first_write_seen = 0U;
	current_runtime()->audio_ahi_route = 0;
	current_runtime()->audio_host_tail_seen = 0U;
#if ZZAPP_PERF_COUNTERS
	current_runtime()->audio_low_water_latched = 0U;
#endif
	zzapp_kernel_audio_close(zzapp_runtime_kernel(), service_slot());
	zzapp_proc_slot(service_slot())->audio_direct = 0U;
	if (have_window()) {
		memset(current_window() + ZZAPP_AUDIO_OFF, 0, ZZAPP_AUDIO_HDR);
		win_flush(ZZAPP_AUDIO_OFF, ZZAPP_AUDIO_HDR);
		life_put(ZZAPP_LIFE_AUDIO_ROUTE_OFF, ZZAPP_AUDIO_ROUTE_CARD);
		life_put(ZZAPP_LIFE_AUDIO_RATE_OFF, 0U);
		life_put(ZZAPP_LIFE_AUDIO_CHANNELS_OFF, 0U);
		life_put(ZZAPP_LIFE_AUDIO_FORMAT_OFF, 0U);
		life_put(ZZAPP_LIFE_AUDIO_UNDERRUNS_OFF, 0U);
	}
}

static int audio_attach(void)
{
	uint32_t slot = 0U;
	uint32_t bytes = 8U + ZZAPP_AUDIO_FRAMES * 4U;
	uint32_t alloc_bytes = (bytes + 4095U) & ~4095U;
	current_runtime()->audio_ahi_route = (life_get(ZZAPP_LIFE_FLAGS_OFF) &
			   ZZAPP_F_LAUNCHER_AUDIO_AHI) != 0U;
	if (current_runtime()->audio_open)
		return AAR_E_BUSY;
	if (!current_runtime()->audio_ahi_route) {
		slot = AUDIO_FABRIC_SLOT_RESERVED;
		if (slot_is_busy(slot))
			slot = AUDIO_FABRIC_SLOT_MAILBOX;
		if (slot_is_busy(slot))
			return AAR_E_BUSY;
	}
#ifdef ZZAPP_HOST_TEST
	current_runtime()->audio_block = aligned_alloc(4096U, alloc_bytes);
	if (!current_runtime()->audio_block)
		return AAR_E_NOMEM;
	memset(current_runtime()->audio_block, 0, alloc_bytes);
#else
	if (!current_runtime()->audio_ahi_route &&
	    !audio_fabric_conversion_admissible(ZZAPP_AUDIO_RATE))
		return AAR_E_BUSY;
	current_runtime()->audio_block = (void *)(uintptr_t)surface_allocator_alloc_range(
		alloc_bytes, 4096U, SDK_MAILBOX_Z3_RESERVE_END,
		SDK_AUDIO_DIRECT_RING_Z3_RESERVE_ADDRESS);
	if (!current_runtime()->audio_block)
		return AAR_E_NOMEM;
#endif
	if (grant_user_buffer((uint32_t)(uintptr_t)current_runtime()->audio_block,
			      bytes, &current_runtime()->audio_grant_base,
			      &current_runtime()->audio_grant_bytes,
			      &current_runtime()->audio_alias) != AAR_OK) {
#ifdef ZZAPP_HOST_TEST
		free(current_runtime()->audio_block);
#else
		surface_allocator_free((uint32_t)(uintptr_t)current_runtime()->audio_block);
#endif
		current_runtime()->audio_block = NULL;
		return AAR_E_NOMEM;
	}
	current_runtime()->audio_read_frame = (volatile uint32_t *)current_runtime()->audio_block;
	current_runtime()->audio_write_frame = current_runtime()->audio_read_frame + 1;
	current_runtime()->audio_samples = (int16_t *)(current_runtime()->audio_read_frame + 2);
	*current_runtime()->audio_read_frame = 0U;
	*current_runtime()->audio_write_frame = 0U;
	current_runtime()->audio_staged = 0U;
	current_runtime()->audio_ahi_staged_frame = 0U;
	current_runtime()->audio_first_write_seen = 0U;
	current_runtime()->audio_slot = slot;
	current_runtime()->audio_host_tail_seen = 0U;
#if ZZAPP_PERF_COUNTERS
	current_runtime()->audio_low_water_latched = 0U;
#endif
	if (current_runtime()->audio_ahi_route) {
		win_put32(ZZAPP_AUDIO_OFF + ZZAPP_AUDIO_HEAD_OFF, 0U);
		win_put32(ZZAPP_AUDIO_OFF + ZZAPP_AUDIO_TAIL_OFF, 0U);
		win_put32(ZZAPP_AUDIO_OFF + ZZAPP_AUDIO_RATE_OFF, ZZAPP_AUDIO_RATE);
		win_put32(ZZAPP_AUDIO_OFF + ZZAPP_AUDIO_CHANNELS_OFF, 2U);
		win_put32(ZZAPP_AUDIO_OFF + ZZAPP_AUDIO_FORMAT_OFF,
			  ZZAPP_AUDIO_FORMAT_S16LE);
		win_put32(ZZAPP_AUDIO_OFF + ZZAPP_AUDIO_ROUTE_OFF,
			  ZZAPP_AUDIO_ROUTE_AHI);
		win_put32(ZZAPP_AUDIO_OFF + ZZAPP_AUDIO_UNDERRUNS_OFF, 0U);
		life_put(ZZAPP_LIFE_AUDIO_ROUTE_OFF, ZZAPP_AUDIO_ROUTE_AHI);
		life_put(ZZAPP_LIFE_AUDIO_RATE_OFF, ZZAPP_AUDIO_RATE);
		life_put(ZZAPP_LIFE_AUDIO_CHANNELS_OFF, 2U);
		life_put(ZZAPP_LIFE_AUDIO_FORMAT_OFF, ZZAPP_AUDIO_FORMAT_S16LE);
	}
	/* The ISR may snapshot as soon as go_live returns. */
	current_runtime()->audio_open = 1;
	barrier();
#ifndef ZZAPP_HOST_TEST
	if (!current_runtime()->audio_ahi_route) {
		if (!audio_fabric_producer_attach(slot, app_audio_ops_for_slot(slot))) {
			current_runtime()->audio_open = 0;
			revoke_audio_grant();
			surface_allocator_free((uint32_t)(uintptr_t)current_runtime()->audio_block);
			current_runtime()->audio_block = NULL;
			current_runtime()->audio_samples = NULL;
			current_runtime()->audio_read_frame = NULL;
			current_runtime()->audio_write_frame = NULL;
			return AAR_E_BUSY;
		}
		audio_fabric_producer_rate_set(slot, ZZAPP_AUDIO_RATE);
	}
#endif
	if (have_window()) {
		uint32_t epoch = current_runtime()->audio_stream_epoch + 1U;

		if (epoch == 0U)
			epoch = 1U;
		current_runtime()->audio_stream_epoch = epoch;
		/* Last: the launcher ignores the header until this is nonzero. */
		barrier();
		win_put32(ZZAPP_AUDIO_OFF + ZZAPP_AUDIO_EPOCH_OFF, epoch);
	}
	return AAR_OK;
}

static void audio_fill(aar_audio *out)
{
	uintptr_t alias = current_runtime()->audio_alias;

	out->samples = (int16_t *)(alias + 8U);
	out->frames = ZZAPP_AUDIO_FRAMES;
	out->read_frame = (volatile uint32_t *)alias;
	out->write_frame = (volatile uint32_t *)(alias + 4U);
	out->rate = ZZAPP_AUDIO_RATE;
	out->channels = 2U;
	out->flags = 0U;
}

int zzapp_svc_audio_open(uint32_t rate, uint32_t channels, aar_audio *out)
{
	int direct_card_route;
	int rc;

	if (!out)
		return AAR_E_INVAL;
	if (channels != 2U || rate != ZZAPP_AUDIO_RATE)
		return AAR_E_INVAL;
	if (current_runtime()->audio_open || current_runtime()->audio_request != AUDIO_REQUEST_NONE)
		return AAR_E_BUSY;
	direct_card_route = (life_get(ZZAPP_LIFE_FLAGS_OFF) &
			     ZZAPP_F_LAUNCHER_AUDIO_AHI) == 0U;
	if (zzapp_kernel_audio_open(zzapp_runtime_kernel(), service_slot(),
				    direct_card_route) != 0)
		return AAR_E_BUSY;
#ifdef ZZAPP_HOST_TEST
	rc = audio_attach();
#else
	current_runtime()->audio_result = AAR_E_BUSY;
	barrier();
	current_runtime()->audio_request = AUDIO_REQUEST_OPEN;
	wait_begin(ZZAPP_WAIT_AUDIO, 0U, 0U);
	while (current_runtime()->audio_request != AUDIO_REQUEST_NONE) {
		yield_wait_once();
		if (wait_status() != AAR_OK) {
			wait_end();
			return wait_status();
		}
	}
	wait_end();
	rc = current_runtime()->audio_result;
#endif
	if (rc != AAR_OK) {
		zzapp_kernel_audio_close(zzapp_runtime_kernel(), service_slot());
		return rc;
	}
	zzapp_proc_slot(service_slot())->audio_direct = direct_card_route ? 1U : 0U;
	audio_fill(out);
	return AAR_OK;
}

int zzapp_svc_audio_close(void)
{
	if (!current_runtime()->audio_open || current_runtime()->audio_request != AUDIO_REQUEST_NONE)
		return AAR_E_INVAL;
#ifdef ZZAPP_HOST_TEST
	audio_detach();
#else
	current_runtime()->audio_result = AAR_E_IO;
	barrier();
	current_runtime()->audio_request = AUDIO_REQUEST_CLOSE;
	wait_begin(ZZAPP_WAIT_AUDIO, 0U, 0U);
	while (current_runtime()->audio_request != AUDIO_REQUEST_NONE) {
		yield_wait_once();
		if (wait_status() != AAR_OK) {
			wait_end();
			return wait_status();
		}
	}
	wait_end();
	if (current_runtime()->audio_result != AAR_OK)
		return current_runtime()->audio_result;
#endif
	return AAR_OK;
}

static void audio_post_teardown(void)
{
	/* Force-stop and fault frames cannot wait for core 0. Post the same
	 * close handoff explicit close uses, and leave the slot busy until
	 * core 0 (or a halted-core reclaim) finishes the detach. */
	if (current_runtime()->audio_request == AUDIO_REQUEST_CLOSE ||
	    current_runtime()->audio_request == AUDIO_REQUEST_CLOSE_QUIESCED) {
		current_runtime()->audio_teardown = 1;
		return;
	}
	if (!current_runtime()->audio_open && !current_runtime()->audio_block &&
	    current_runtime()->audio_request != AUDIO_REQUEST_OPEN)
		return;
	current_runtime()->audio_teardown = 1;
	current_runtime()->audio_result = AAR_E_IO;
	barrier();
	if (current_runtime()->audio_request != AUDIO_REQUEST_OPEN)
		current_runtime()->audio_request = AUDIO_REQUEST_CLOSE;
}

static int audio_handoff_pending(const struct zzapp_service_runtime *state)
{
	return state->audio_teardown ||
	       state->audio_request == AUDIO_REQUEST_CLOSE ||
	       state->audio_request == AUDIO_REQUEST_CLOSE_QUIESCED;
}

static void finish_audio_slot_release(void)
{
	slot_state_lock_acquire();
	if (slots[service_slot()].state == ZZAPP_ST_STOPPING &&
	    defer_load() == DEFER_IDLE &&
	    !audio_handoff_pending(current_runtime()))
		publish_claim_idle(&slots[service_slot()]);
	slot_state_lock_release();
}

static void reclaim_halted_audio_slot(void)
{
	if (current_runtime()->audio_request == AUDIO_REQUEST_OPEN) {
		current_runtime()->audio_result = AAR_E_IO;
		current_runtime()->audio_teardown = 0;
		barrier();
		current_runtime()->audio_request = AUDIO_REQUEST_NONE;
		finish_audio_slot_release();
		return;
	}
	if (!current_runtime()->audio_open && !current_runtime()->audio_block &&
	    current_runtime()->audio_request != AUDIO_REQUEST_CLOSE &&
	    current_runtime()->audio_request != AUDIO_REQUEST_CLOSE_QUIESCED) {
		finish_audio_slot_release();
		return;
	}
	audio_detach();
	current_runtime()->audio_teardown = 0;
	current_runtime()->audio_result = AAR_OK;
	barrier();
	current_runtime()->audio_request = AUDIO_REQUEST_NONE;
	finish_audio_slot_release();
}

void zzapp_services_reclaim_halted_audio(void)
{
	uint32_t slot;
	uint32_t selected = selected_slot[service_context()];

	for (slot = 0U; slot < ZZAPP_SLOT_COUNT; slot++) {
		select_slot(slot);
		reclaim_halted_audio_slot();
	}
	select_slot(selected);
}

/* The Amiga rings no doorbell, so a parked EVENT waiter is a level test of
 * the host-to-app ring on every service pass; a push that landed before the
 * app finished parking is found here. APP_STOP only latches the stop word,
 * so a stop wakes the waiter too, and it then reports AAR_E_IO. */
static int event_wait_ready(void)
{
	return win_get32(ZZAPP_MSG_H2A_OFF) !=
		       win_get32(ZZAPP_MSG_H2A_OFF + 4U) ||
	       zzapp_services_stop_requested();
}

static void wake_ready_waits(void)
{
	uint32_t slot;
	uint32_t selected = selected_slot[service_context()];

	for (slot = 0U; slot < ZZAPP_SLOT_COUNT; slot++) {
		struct zzapp_service_runtime *state = runtime_for_slot(slot);
		int ready = 0;

		select_slot(slot);
		if (state->wait_kind == ZZAPP_WAIT_NONE)
			continue;
		/* An arrived message or a stop beats the deadline: the waiter
		 * reports a message that landed while it was timing out. */
		if (state->wait_kind == ZZAPP_WAIT_EVENT &&
		    event_wait_ready()) {
			(void)zzapp_kernel_wake(zzapp_runtime_kernel(), slot,
						 ZZAPP_WAKE_READY);
			continue;
		}
		if (state->wait_deadline != 0U && now_us() >= state->wait_deadline) {
			(void)zzapp_kernel_wake(zzapp_runtime_kernel(), slot,
						 ZZAPP_WAKE_TIMEOUT);
			continue;
		}
		switch (state->wait_kind) {
		case ZZAPP_WAIT_FILE:
			ready = win_get32(ZZAPP_FILE_OFF) == state->wait_value;
			break;
		case ZZAPP_WAIT_CONSOLE_WRITE:
			ready = win_get32(ZZAPP_CONSOLE_OUT_OFF + 4U) !=
				state->wait_value;
			break;
		case ZZAPP_WAIT_CONSOLE_READ:
			ready = win_get32(ZZAPP_CONSOLE_IN_OFF) != state->wait_value ||
				win_get32(ZZAPP_CONSOLE_IN_OFF +
					  ZZAPP_CONSOLE_EOF_OFF) != 0U;
			break;
		case ZZAPP_WAIT_DISPLAY_OPEN:
			ready = state->display_bound ||
				(win_get32(ZZAPP_LIFE_OFF + ZZAPP_LIFE_FLAGS_OFF) &
				 ZZAPP_F_DISPLAY_UNBOUND) == 0U;
			break;
		case ZZAPP_WAIT_FLIP:
			ready = win_get32(ZZAPP_LIFE_OFF + ZZAPP_LIFE_FLIP_ACK_OFF) ==
				state->wait_value;
			break;
		case ZZAPP_WAIT_AUDIO:
			ready = state->audio_request == AUDIO_REQUEST_NONE;
			break;
		case ZZAPP_WAIT_EVENT:
			break;
		default:
			break;
		}
		if (ready)
			(void)zzapp_kernel_wake(zzapp_runtime_kernel(), slot,
						 ZZAPP_WAKE_READY);
	}
	select_slot(selected);
}

static void audio_core0_poll_slot(void)
{
	uint32_t request = current_runtime()->audio_request;

	if (request == AUDIO_REQUEST_OPEN) {
		if (current_runtime()->audio_teardown)
			current_runtime()->audio_result = AAR_E_IO;
		else
			current_runtime()->audio_result = audio_attach();
		barrier();
		current_runtime()->audio_request = AUDIO_REQUEST_NONE;
		if (current_runtime()->audio_teardown &&
		    (current_runtime()->audio_open || current_runtime()->audio_block))
			current_runtime()->audio_request = AUDIO_REQUEST_CLOSE;
		else
			current_runtime()->audio_teardown = 0;
		finish_audio_slot_release();
		return;
	}
	if (request == AUDIO_REQUEST_NONE) {
#ifndef ZZAPP_HOST_TEST
		if (current_runtime()->audio_first_write_seen &&
		    current_runtime()->audio_open &&
		    !current_runtime()->audio_ahi_route) {
			(void)audio_fabric_producer_go_live_when_ready(
				current_runtime()->audio_slot, 1);
		}
#endif
		finish_audio_slot_release();
		return;
	}
	if (request != AUDIO_REQUEST_CLOSE_QUIESCED) {
		finish_audio_slot_release();
		return;
	}
	audio_detach();
	current_runtime()->audio_teardown = 0;
	current_runtime()->audio_result = AAR_OK;
	barrier();
	current_runtime()->audio_request = AUDIO_REQUEST_NONE;
	finish_audio_slot_release();
}

void zzapp_services_core0_poll(void)
{
	uint32_t slot;
	uint32_t selected = selected_slot[service_context()];

	for (slot = 0U; slot < ZZAPP_SLOT_COUNT; slot++) {
		select_slot(slot);
		audio_core0_poll_slot();
	}
	select_slot(selected);
}

static void init_window(uint8_t *base, uint32_t bytes, int uncached)
{
	uint32_t slot = service_slot();

	slots[slot].window = base;
	slots[slot].window_bytes = bytes;
	slots[slot].window_uncached = uncached;
	current_runtime()->file_seq = 0U;
	memset(current_window(), 0, ZZAPP_WINDOW_BYTES);
	win_flush(0U, ZZAPP_WINDOW_BYTES);
	life_put(ZZAPP_LIFE_MAGIC_OFF, ZZAPP_LIFE_MAGIC);
	life_put(ZZAPP_LIFE_VERSION_OFF, ZZAPP_LIFE_VERSION);
	life_put(ZZAPP_LIFE_STATE_OFF, ZZAPP_ST_IDLE);
}

static int alloc_window(uint32_t slot, uint32_t *arm_addr, uint32_t *host_off)
{
	select_slot(slot);
#ifdef ZZAPP_HOST_TEST
	if (!have_window()) {
		uint32_t serial = 0U;

		if (!test_window_backing[slot])
			return AAR_E_NOMEM;
		if (zzapp_test_card_alloc_at) {
			if (!zzapp_test_card_alloc_at(
				    ZZAPP_WINDOW_Z3_SLOT_ADDRESS(slot),
				    ZZAPP_WINDOW_BYTES, &serial))
				return AAR_E_NOMEM;
		} else {
			if (test_window_range_held[slot])
				return AAR_E_NOMEM;
			test_window_range_held[slot] = 1;
		}
		test_window_serial[slot] = serial;
		init_window(test_window_backing[slot], ZZAPP_WINDOW_BYTES, 1);
	}
	*arm_addr = (uint32_t)(uintptr_t)current_window();
	*host_off = *arm_addr;
	return AAR_OK;
#else
	uint32_t addr;

	if (have_window()) {
		*arm_addr = (uint32_t)(uintptr_t)current_window();
		*host_off = *arm_addr - ADDR_ADJ;
		return AAR_OK;
	}
	if (!window_mapping_available)
		return AAR_E_NOMEM;
	/* Each slot owns a fixed range that main.c mapped non-cacheable; the
	 * allocator keeps other host-visible users out of it. */
	addr = ZZAPP_WINDOW_Z3_SLOT_ADDRESS(slot);
	addr = surface_allocator_alloc_range(ZZAPP_WINDOW_BYTES, 4096U, addr,
					     addr + ZZAPP_WINDOW_Z3_SIZE);
	if (addr == 0U)
		return AAR_E_NOMEM;
	init_window((uint8_t *)(uintptr_t)addr, ZZAPP_WINDOW_BYTES, 1);
	*arm_addr = addr;
	*host_off = addr - ADDR_ADJ;
	return AAR_OK;
#endif
}

static int bind_surface(const uint8_t *req, uint16_t req_len)
{
	uint32_t width;
	uint32_t height;
	uint32_t pitch;
	uint32_t format;
	uint32_t front;
	uint32_t back;
	uint32_t spare = 0U;
	uint32_t bpp;
	int rc;
	uint32_t layout;
	uint32_t bytes;
	uint32_t flags;

	if (current_runtime()->display_bound)
		return AAR_E_BUSY;
	if (req_len < ZZAPP_BIND_CARD_BACK + sizeof(uint32_t))
		return AAR_E_INVAL;
	width = zzapp_get_be32(req + ZZAPP_BIND_WIDTH);
	height = zzapp_get_be32(req + ZZAPP_BIND_HEIGHT);
	pitch = zzapp_get_be32(req + ZZAPP_BIND_PITCH);
	format = zzapp_get_be32(req + ZZAPP_BIND_FORMAT);
	front = zzapp_get_be32(req + ZZAPP_BIND_CARD_FRONT);
	back = zzapp_get_be32(req + ZZAPP_BIND_CARD_BACK);
	layout = req_len >= ZZAPP_BIND_REQ_BYTES ?
		zzapp_get_be32(req + ZZAPP_BIND_LAYOUT) : default_layout(format);
	if (req_len >= ZZAPP_BIND_EXT_REQ_BYTES)
		spare = zzapp_get_be32(req + ZZAPP_BIND_CARD_SPARE);
	bpp = format_bpp(format);
	if (!layout_matches_format(format, layout))
		return AAR_E_INVAL;
	if (width == 0U || height == 0U || width > 4096U || height > 4096U ||
	    bpp == 0U || pitch < width * bpp || height > (0xffffffffU / pitch))
		return AAR_E_INVAL;
	bytes = pitch * height;
	if (!span_ok(front, bytes))
		return AAR_E_INVAL;
	if (spare != 0U && (!span_ok(spare, bytes) || spare == front || spare == back))
		return AAR_E_INVAL;
	drop_owned_back();
	if (back == 0U) {
		rc = zzapp_loader_alloc_back(bytes, &back);
		if (rc != AAR_OK)
			return rc == AAR_E_NOSYS ? AAR_E_NOMEM : rc;
		current_runtime()->display_back_owned = 1;
	} else if (!span_ok(back, bytes)) {
		return AAR_E_INVAL;
	} else {
		current_runtime()->display_back_owned = 0;
	}
	if (grant_surface_buffers(front, back, spare, bytes) != AAR_OK) {
		release_unbound_back(back);
		return AAR_E_NOMEM;
	}
	{
		uint32_t buffers[3] = { front, back, spare };
		uint32_t lease_base = front;
		uint64_t lease_end = (uint64_t)front + bytes;
		uint32_t lease_bytes;
		uint32_t i;

		for (i = 1U; i < 3U; i++) {
			if (buffers[i] == 0U)
				continue;
			if (buffers[i] < lease_base)
				lease_base = buffers[i];
			if ((uint64_t)buffers[i] + bytes > lease_end)
				lease_end = (uint64_t)buffers[i] + bytes;
		}
		if (lease_end > UINT32_MAX) {
			revoke_surface_grants();
			release_unbound_back(back);
			return AAR_E_INVAL;
		}
		lease_base &= ~(ZZAPP_KERNEL_PAGE_BYTES - 1U);
		lease_bytes = ((uint32_t)lease_end - lease_base +
			       ZZAPP_KERNEL_PAGE_BYTES - 1U) &
			~(ZZAPP_KERNEL_PAGE_BYTES - 1U);
		if (zzapp_kernel_surface_bind(zzapp_runtime_kernel(), service_slot(),
					      zzapp_services_generation(), lease_base,
					      lease_bytes) != 0) {
			revoke_surface_grants();
			release_unbound_back(back);
			return AAR_E_BUSY;
		}
	}

	life_put(ZZAPP_LIFE_WIDTH_OFF, width);
	life_put(ZZAPP_LIFE_HEIGHT_OFF, height);
	life_put(ZZAPP_LIFE_PITCH_OFF, pitch);
	life_put(ZZAPP_LIFE_FORMAT_OFF, format);
	life_put(ZZAPP_LIFE_CARD_FRONT_OFF, front);
	life_put(ZZAPP_LIFE_CARD_BACK_OFF, back);
	life_put(ZZAPP_LIFE_LAYOUT_OFF, layout);
	life_put(ZZAPP_LIFE_CARD_SPARE_OFF, spare);
	life_put(ZZAPP_LIFE_FLIP_SHOWING_OFF, 0U);
	life_put(ZZAPP_LIFE_FLIP_ADDRESS_OFF, 0U);
	life_put(ZZAPP_LIFE_FLIP_SUBMIT_OFF, 0U);
	life_put(ZZAPP_LIFE_MAILBOX_REPLACED_OFF, 0U);
	current_runtime()->display_width = width;
	current_runtime()->display_height = height;
	current_runtime()->display_pitch = pitch;
	current_runtime()->display_format = format;
	current_runtime()->display_layout = layout;
	current_runtime()->display_bytes = bytes;
	current_runtime()->display_front = front;
	current_runtime()->display_back = back;
	current_runtime()->display_spare = spare;
	current_runtime()->display_present = 0U;
	current_runtime()->display_flip_gen = life_get(ZZAPP_LIFE_FLIP_GEN_OFF);
	current_runtime()->display_ack_seen = life_get(ZZAPP_LIFE_FLIP_ACK_OFF);
	current_runtime()->display_bound = 1;
	current_runtime()->display_id = 1U;
	display_mailbox_disable();
	flags = zzapp_services_flags();
	if ((flags & (ZZAPP_F_MAILBOX_REQUEST | ZZAPP_F_LAUNCHER_MAILBOX)) ==
	    (ZZAPP_F_MAILBOX_REQUEST | ZZAPP_F_LAUNCHER_MAILBOX) &&
	    !current_runtime()->display_back_owned && spare != 0U) {
		display_mailbox_lock_acquire();
		current_runtime()->display_mailbox_shown = front;
		current_runtime()->display_mailbox_pending = 0U;
		current_runtime()->display_mailbox_buffers[0] = front;
		current_runtime()->display_mailbox_buffers[1] = back;
		current_runtime()->display_mailbox_buffers[2] = spare;
		current_runtime()->display_mailbox_bytes = bytes;
		current_runtime()->display_mailbox_latch_pending = 0;
		__atomic_store_n(&current_runtime()->display_mailbox_active, 1, __ATOMIC_RELEASE);
		display_mailbox_lock_release();
		zzapp_services_or_flags(ZZAPP_F_MAILBOX);
	} else {
		zzapp_services_and_flags(~ZZAPP_F_MAILBOX);
	}
	barrier();
	if (zzapp_services_flags() & ZZAPP_F_LAUNCHER_SCANOUT_ACK)
		zzapp_services_or_flags(ZZAPP_F_SCANOUT_ACK);
	else
		zzapp_services_and_flags(~ZZAPP_F_SCANOUT_ACK);
	/* Last: the app and ZZRun wait for this bit to clear before using the
	 * surface, so every field above must already be published. */
	zzapp_services_and_flags(~ZZAPP_F_DISPLAY_UNBOUND);
	return AAR_OK;
}

static void capture_start_name(uint32_t argc)
{
	uint8_t *name = slots[service_slot()].name;
	uint32_t off;
	uint32_t end;
	uint32_t start;
	uint32_t i;
	uint32_t length;

	memset(name, 0, ZZAPP_LIST_NAME_BYTES);
	if (argc == 0U || !have_window())
		return;
	off = win_get32(ZZAPP_ARGV_OFF + ZZAPP_ARGV_OFFS_OFF);
	if (off < ZZAPP_ARGV_OFF + ZZAPP_ARGV_STR_OFF ||
	    off >= ZZAPP_ARGV_OFF + ZZAPP_ARGV_BYTES)
		return;
	start = off;
	length = ZZAPP_ARGV_OFF + ZZAPP_ARGV_BYTES - off;
	if (length > ZZAPP_ARGV_STR_MAX)
		length = ZZAPP_ARGV_STR_MAX;
	end = 0U;
	for (i = 0U; i < length && current_window()[start + i] != '\0'; i++) {
		if (current_window()[start + i] == '/' ||
		    current_window()[start + i] == ':')
			end = i + 1U;
	}
	for (i = end; i + 1U < ZZAPP_LIST_NAME_BYTES + end && i < length; i++)
		name[i - end] = current_window()[start + i];
}

static void audio_core1_poll_all(void)
{
	uint32_t slot;
	uint32_t selected = selected_slot[service_context()];

#if ZZAPP_PERF_COUNTERS && !defined(ZZAPP_HOST_TEST)
	zzapp_kernel_perf_service_begin(zzapp_runtime_kernel(),
					ZZAPP_PERF_SERVICE_AUDIO,
					scheduler_perf_stamp());
#endif
	for (slot = 0U; slot < ZZAPP_SLOT_COUNT; slot++) {
		select_slot(slot);
		if (current_runtime()->audio_request == AUDIO_REQUEST_CLOSE) {
			barrier();
			current_runtime()->audio_request =
				AUDIO_REQUEST_CLOSE_QUIESCED;
			continue;
		}
		if (current_runtime()->audio_request != AUDIO_REQUEST_NONE)
			continue;
		audio_pump_ahi_window();
#ifndef ZZAPP_HOST_TEST
		if (current_runtime()->audio_open &&
		    !current_runtime()->audio_ahi_route &&
		    current_runtime()->audio_write_frame &&
		    *current_runtime()->audio_write_frame != 0U) {
			barrier();
			current_runtime()->audio_first_write_seen = 1U;
		}
#endif
	}
#if ZZAPP_PERF_COUNTERS && !defined(ZZAPP_HOST_TEST)
	zzapp_kernel_perf_service_end(zzapp_runtime_kernel(),
				      ZZAPP_PERF_SERVICE_AUDIO,
				      scheduler_perf_stamp());
#endif
	select_slot(selected);
}

static void complete_cancelled_deferred(struct zzapp_deferred *defer)
{
	int release;

	slot_state_lock_acquire();
	release = defer->cancelled && defer->state == DEFER_RUNNING;
	slot_state_lock_release();
	if (!release)
		return;
	/*
	 * LOAD_BEGIN can be cancelled after core 1 takes its deferred record but
	 * before the loader owns an arena; its abort then returns EBUSY. Either
	 * way this worker has finished the only teardown the slot needs, so mark
	 * it reclaimed. The release acknowledgement happens once, as the last
	 * state write of run_app/start_failed, never here: acknowledging inline
	 * could publish IDLE and open admission mid-teardown.
	 */
	slot_state_lock_acquire();
	if (slots[service_slot()].quarantined) {
		memset(current_runtime(), 0, sizeof(*current_runtime()));
		slots[service_slot()].quarantine_reclaimed = 1;
	}
	slot_state_lock_release();
	(void)zzapp_loader_abort();
	slot_state_lock_acquire();
	if (defer->cancelled && defer->state == DEFER_RUNNING) {
		defer->start_pending = 0U;
		memset(current_runtime(), 0, sizeof(*current_runtime()));
		publish_claim_idle(&slots[service_slot()]);
		defer->state = DEFER_IDLE;
	}
	slot_state_lock_release();
}

static void discard_deferred_run(struct zzapp_deferred *defer)
{
	int cancelled;

	slot_state_lock_acquire();
	cancelled = defer->cancelled;
	if (!cancelled && defer->state == DEFER_RUNNING)
		defer->state = DEFER_IDLE;
	slot_state_lock_release();
	if (cancelled)
		complete_cancelled_deferred(defer);
}

static int finish_deferred(struct zzapp_deferred *defer, int rc,
			   uint32_t publish_state)
{
#ifdef ZZAPP_SERVICES_STANDALONE_TEST
	if (service_test_deferred_completing_hook)
		service_test_deferred_completing_hook();
#endif
	slot_state_lock_acquire();
	if (defer->cancelled ||
	    defer->generation != slots[service_slot()].generation ||
	    defer->state != DEFER_RUNNING) {
		slot_state_lock_release();
		complete_cancelled_deferred(defer);
		return 0;
	}
	defer->status = zz_to_sdk(rc);
	defer->reply_len = 0U;
	if (publish_state == ZZAPP_ST_LOADING)
		slots[service_slot()].load_progress = now_us();
	if (publish_state != UINT32_MAX)
		set_state_locked(publish_state);
#ifdef ZZAPP_SERVICES_STANDALONE_TEST
	if (service_test_deferred_reply_hook) {
		slot_state_lock_release();
		service_test_deferred_reply_hook();
		slot_state_lock_acquire();
		if (defer->cancelled ||
		    defer->generation != slots[service_slot()].generation ||
		    defer->state != DEFER_RUNNING) {
			slot_state_lock_release();
			complete_cancelled_deferred(defer);
			return 0;
		}
	}
#endif
	__atomic_store_n(&defer->state, DEFER_DONE, __ATOMIC_RELEASE);
	slot_state_lock_release();
	return 1;
}

static void run_deferred(void)
{
	struct zzapp_deferred *defer = current_deferred();
	int rc = AAR_E_INVAL;
	const char *reject_reason = NULL;
	const uint8_t *req = defer->req;
	uint16_t len = defer->req_len;
	uint32_t publish_state = UINT32_MAX;

	if (defer_load() != DEFER_RUNNING) {
		complete_cancelled_deferred(defer);
		return;
	}
	defer->reply_len = 0U;
	if (defer_cancelled(defer) ||
	    defer->generation != slots[service_slot()].generation) {
		discard_deferred_run(defer);
		return;
	}
	switch (defer->opcode) {
	case ZZAPP_OP_LOAD_BEGIN:
		if (zzapp_services_state() != ZZAPP_ST_IDLE &&
		    zzapp_services_state() != ZZAPP_ST_CLAIMED) {
			rc = AAR_E_BUSY;
			break;
		}
		if (slots[service_slot()].legacy_claim ||
		    slots[service_slot()].generation == 0U) {
			uint32_t generation = slots[service_slot()].generation + 1U;

			if (generation == 0U)
				generation = 1U;
			slots[service_slot()].generation = generation;
			defer->generation = generation;
			life_put(ZZAPP_LIFE_GENERATION_OFF, generation);
		}
		if (defer_load() != DEFER_RUNNING || defer_cancelled(defer)) {
			discard_deferred_run(defer);
			return;
		}
		rc = zzapp_loader_set_generation(slots[service_slot()].generation);
		if (rc != AAR_OK) {
			if (rc == AAR_E_INVAL)
				reject_reason = "loader arena remains bound";
			break;
		}
		rc = zzapp_loader_begin();
		if (rc == AAR_OK)
			publish_state = ZZAPP_ST_LOADING;
		if (rc == AAR_OK) {
			slots[service_slot()].load_step = 0U;
			life_put(ZZAPP_LIFE_PROGRESS_LO_OFF, 0U);
			life_put(ZZAPP_LIFE_PROGRESS_HI_OFF, 0U);
		}
		break;
	case ZZAPP_OP_LOAD_CHUNK: {
		uint32_t off;
		uint32_t n;
		uint32_t staging;

		if (len < ZZAPP_CHUNK_REQ_BYTES) {
			reject_reason = "chunk payload is shorter than 12 bytes";
			rc = AAR_E_INVAL;
			break;
		}
		off = zzapp_get_be32(req + ZZAPP_CHUNK_FILE_OFF);
		n = zzapp_get_be32(req + ZZAPP_CHUNK_LENGTH);
		staging = zzapp_get_be32(req + ZZAPP_CHUNK_STAGING);
		if (n < ZZAPP_CHUNK_MIN || n > ZZAPP_CHUNK_MAX) {
			reject_reason = "chunk length is outside staging bounds";
			rc = AAR_E_INVAL;
			break;
		}
		if (staging != ZZAPP_STAGING_OFF) {
			reject_reason = "chunk staging offset does not match";
			rc = AAR_E_INVAL;
			break;
		}
		if (zzapp_services_state() != ZZAPP_ST_LOADING) {
			reject_reason = "chunk received outside loading state";
			rc = AAR_E_INVAL;
			break;
		}
		win_inval(ZZAPP_STAGING_OFF, n);
		rc = zzapp_loader_chunk(off, current_window() + ZZAPP_STAGING_OFF, n);
		if (rc == AAR_OK) {
			slot_state_lock_acquire();
			if (!defer_cancelled(defer) &&
			    defer->generation == slots[service_slot()].generation &&
			    slots[service_slot()].state == ZZAPP_ST_LOADING)
				slots[service_slot()].load_progress = now_us();
			slot_state_lock_release();
		}
		if (rc == AAR_E_INVAL)
			reject_reason = "loader rejected chunk offset or range";
		break;
	}
	case ZZAPP_OP_LOAD_END:
		if (zzapp_services_state() != ZZAPP_ST_LOADING) {
			reject_reason = "load end received outside loading state";
			rc = AAR_E_INVAL;
			break;
		}
		rc = zzapp_loader_end();
		if (rc == ZZAPP_LOADER_IN_PROGRESS) {
			uint32_t step;

			slot_state_lock_acquire();
			slots[service_slot()].load_progress = now_us();
			step = ++slots[service_slot()].load_step;
			slot_state_lock_release();
			life_put(ZZAPP_LIFE_PROGRESS_LO_OFF, step);
			life_put(ZZAPP_LIFE_PROGRESS_HI_OFF, 0U);
			return;
		}
		if (rc == AAR_E_PROFILE)
			reject_reason = "note profile is not A32";
		else if (rc == AAR_E_NOGROUP)
			reject_reason = "note requires a group this runtime lacks";
		if (rc != AAR_OK)
			publish_state = ZZAPP_ST_IDLE;
		break;
	case ZZAPP_OP_START:
		if (zzapp_services_state() != ZZAPP_ST_LOADING &&
		    zzapp_services_state() != ZZAPP_ST_STARTING) {
			reject_reason = "start received outside loading state";
			rc = AAR_E_INVAL;
			break;
		}
		defer->start_argc = len >= ZZAPP_START_REQ_BYTES
			? zzapp_get_be32(req + ZZAPP_START_ARGC) : 0U;
		if (defer->start_argc > ZZAPP_ARGV_MAX) {
			reject_reason = "start argc exceeds argv capacity";
			rc = AAR_E_INVAL;
			break;
		}
		capture_start_name(defer->start_argc);
		if (defer_load() != DEFER_RUNNING || defer_cancelled(defer)) {
			discard_deferred_run(defer);
			return;
		}
		defer->start_pending = 1U;
		return;
	default:
		rc = AAR_E_NOSYS;
		break;
	}
	if ((zz_to_sdk(rc) == SDK_STATUS_BAD_REQUEST || rc == AAR_E_NOGROUP) &&
	    reject_reason) {
		if (defer->opcode == ZZAPP_OP_LOAD_CHUNK)
			report_load_snapshot("reject", defer->opcode, reject_reason);
		else
			report_rejected_app_op(defer->opcode, reject_reason);
	}
#ifdef ZZAPP_SERVICES_STANDALONE_TEST
	if (service_test_deferred_completion_hook)
		service_test_deferred_completion_hook();
#endif
	if (finish_deferred(defer, rc, publish_state) && rc == AAR_OK &&
	    defer->opcode == ZZAPP_OP_LOAD_BEGIN)
		report_load_snapshot("load begin", defer->opcode, NULL);
}

void zzapp_services_poll(void)
{
	uint32_t checked;
	uint32_t selected = selected_slot[service_context()];

	if (!defer_on_core1()) {
#ifndef ZZAPP_HOST_TEST
		if (!defer_wrong_core_logged) {
			defer_wrong_core_logged = 1U;
			printf("[app] deferred poll ignored on core 0\r\n");
		}
#endif
		return;
	}
	if (zzapp_kernel_current_thread() < 0) {
		for (checked = 0U; checked < ZZAPP_SLOT_COUNT; checked++) {
			uint32_t slot =
				(deferred_poll_next_slot + checked) % ZZAPP_SLOT_COUNT;

			select_slot(slot);
			slot_state_lock_acquire();
			if (current_deferred()->state == DEFER_POSTED ||
			    (current_deferred()->state == DEFER_RUNNING &&
			     current_deferred()->opcode == ZZAPP_OP_LOAD_END)) {
				current_deferred()->state = DEFER_RUNNING;
				deferred_poll_next_slot =
					(slot + 1U) % ZZAPP_SLOT_COUNT;
				slot_state_lock_release();
				run_deferred();
				break;
			}
			slot_state_lock_release();
		}
		select_slot(selected);
	}
	wake_ready_waits();
	audio_core1_poll_all();
}

int zzapp_services_take_start(uint32_t *argc)
{
	struct zzapp_deferred *defer = current_deferred();

	if (!defer->start_pending)
		return 0;
	defer->start_pending = 0U;
	if (argc)
		*argc = defer->start_argc;
	return 1;
}

int zzapp_services_start_entered(uint32_t generation)
{
	struct zzapp_deferred *defer = current_deferred();

	slot_state_lock_acquire();
	if (defer->cancelled || defer->generation != generation ||
	    slots[service_slot()].generation != generation ||
	    defer->state != DEFER_RUNNING || defer->opcode != ZZAPP_OP_START) {
		slot_state_lock_release();
		complete_cancelled_deferred(defer);
		return AAR_E_IO;
	}
	set_state_locked(ZZAPP_ST_RUNNING);
	defer->status = SDK_STATUS_OK;
	defer->reply_len = 0U;
	__atomic_store_n(&defer->state, DEFER_DONE, __ATOMIC_RELEASE);
	slot_state_lock_release();
	return AAR_OK;
}

void zzapp_services_cancel_start(int status)
{
	struct zzapp_deferred *defer = current_deferred();

	slot_state_lock_acquire();
	defer->start_pending = 0U;
	if (defer->state == DEFER_RUNNING && defer->opcode == ZZAPP_OP_START) {
		defer->status = zz_to_sdk(status);
		defer->reply_len = 0U;
		__atomic_store_n(&defer->state, DEFER_DONE, __ATOMIC_RELEASE);
		/* Admission stays closed: the host STARTING teardown reopens it
		 * once the loader arena is gone. */
		if (!slots[service_slot()].quarantined)
			slots[service_slot()].state = ZZAPP_ST_STOPPING;
	}
	slot_state_lock_release();
}

static int queue_deferred(uint16_t opcode, const uint8_t *req, uint16_t req_len,
			  uint32_t request_id, uint32_t user_cookie)
{
	struct zzapp_deferred *defer = current_deferred();

	slot_state_lock_acquire();
	if (defer->state != DEFER_IDLE) {
		slot_state_lock_release();
		return 0;
	}
	defer->state = DEFER_FILLING;
	defer->cancelled = 0U;
	if (req_len > sizeof(defer->req))
		req_len = sizeof(defer->req);
	memset(defer->req, 0, sizeof(defer->req));
	if (req_len != 0U && req)
		memcpy(defer->req, req, req_len);
	defer->opcode = opcode;
	defer->req_len = req_len;
	defer->request_id = request_id;
	defer->cookie = user_cookie;
	defer->generation = slots[service_slot()].generation;
	defer->reply_len = 0U;
	/* The limits belong to the request that was admitted: a LOAD_BEGIN
	 * refused above, or one the slot is no longer in a state to start,
	 * leaves the first load's limits alone. */
	if (opcode == ZZAPP_OP_LOAD_BEGIN &&
	    req_len == ZZAPP_MAILBOX_PAYLOAD_BYTES && req &&
	    (slots[service_slot()].state == ZZAPP_ST_IDLE ||
	     slots[service_slot()].state == ZZAPP_ST_CLAIMED)) {
		slots[service_slot()].memory_limit =
			zzapp_get_be32(req + ZZAPP_LOAD_MEM_LIMIT_MIB) *
			1024U * 1024U;
		slots[service_slot()].host_visible_limit =
			zzapp_get_be32(req + ZZAPP_LOAD_HOST_VISIBLE_MIB) *
			1024U * 1024U;
	}
	defer->state = DEFER_POSTED;
	slot_state_lock_release();
#ifndef ZZAPP_HOST_TEST
	scheduler_core1_publish();
#endif
	return 1;
}

static uint32_t committed_bytes(void)
{
	uint32_t slot;
	uint32_t total = 0U;

	for (slot = 0U; slot < ZZAPP_SLOT_COUNT; slot++) {
		zzapp_proc_t *proc = zzapp_proc_slot(slot);

		if (proc)
			total += proc->memory.committed;
	}
	return total;
}

static void put_query(uint8_t *reply, uint16_t *reply_len, uint32_t caps,
		      int extended)
{
	uint32_t slot;
	uint32_t free_slots = 0U;
	uint32_t committed = committed_bytes();

	memset(reply, 0, extended ? ZZAPP_QUERY_EXT_REPLY_BYTES :
	       ZZAPP_QUERY_REPLY_BYTES);
	zzapp_put_be32(reply + ZZAPP_QUERY_ABI_MAJOR, ZZAPP_ABI_MAJOR);
	zzapp_put_be32(reply + ZZAPP_QUERY_ABI_MINOR, ZZAPP_ABI_MINOR);
	zzapp_put_be32(reply + ZZAPP_QUERY_STATE, slots[0].state);
	zzapp_put_be32(reply + ZZAPP_QUERY_CAPS, caps);
	if (!extended) {
		*reply_len = ZZAPP_QUERY_REPLY_BYTES;
		return;
	}
	for (slot = 0U; slot < ZZAPP_SLOT_COUNT; slot++) {
		if (slots[slot].state == ZZAPP_ST_IDLE &&
		    __atomic_load_n(&deferred[slot].state, __ATOMIC_ACQUIRE) ==
			    DEFER_IDLE)
			free_slots++;
	}
	zzapp_put_be32(reply + ZZAPP_QUERY_SLOTS, ZZAPP_SLOT_COUNT);
	zzapp_put_be32(reply + ZZAPP_QUERY_FREE_SLOTS, free_slots);
	zzapp_put_be32(reply + ZZAPP_QUERY_COMMIT_FREE, 0U);
	zzapp_put_be32(reply + ZZAPP_QUERY_COMMIT_TOTAL, committed);
	*reply_len = ZZAPP_QUERY_EXT_REPLY_BYTES;
}

static int claim_window(uint32_t slot, uint32_t *arm, uint32_t *host,
			int legacy_claim)
{
	uint32_t generation;
	int rc;

	if (slots[slot].state != ZZAPP_ST_IDLE || slots[slot].quarantined ||
	    __atomic_load_n(&deferred[slot].state, __ATOMIC_ACQUIRE) !=
		    DEFER_IDLE ||
	    audio_handoff_pending(runtime_for_slot(slot)))
		return AAR_E_BUSY;
	rc = alloc_window(slot, arm, host);
	if (rc != AAR_OK)
		return rc;
	wait_end();
	slot_state_lock_acquire();
	generation = slots[slot].generation + 1U;
	if (generation == 0U)
		generation = 1U;
	zzapp_kernel_reset_cpu_ticks(zzapp_runtime_kernel(), slot);
	slots[slot].generation = generation;
	slots[slot].legacy_claim = legacy_claim;
	slots[slot].memory_limit = 0U;
	slots[slot].host_visible_limit = 0U;
	slots[slot].terminal_reason = ZZAPP_END_NONE;
	if (zzapp_host_claimed)
		zzapp_host_claimed(slot);
	memset(slots[slot].name, 0, sizeof(slots[slot].name));
	slots[slot].claim_started = now_us();
	slots[slot].load_progress = 0U;
	slots[slot].quarantined = 0;
	slots[slot].quarantine_reclaimed = 0;
	slots[slot].quarantine_release_requested = 0;
	slots[slot].launcher_heartbeat = life_get(ZZAPP_LIFE_HEARTBEAT_OFF);
	slots[slot].state = ZZAPP_ST_CLAIMED;
	life_put(ZZAPP_LIFE_MAGIC_OFF, ZZAPP_LIFE_MAGIC);
	life_put(ZZAPP_LIFE_VERSION_OFF, ZZAPP_LIFE_VERSION);
	life_put(ZZAPP_LIFE_GENERATION_OFF, generation);
	life_put(ZZAPP_LIFE_LOW_MEMORY_CROSSINGS_OFF, 0U);
	if (zzapp_host_reset_low_memory)
		zzapp_host_reset_low_memory(slot);
	life_put(ZZAPP_LIFE_TERMINAL_OFF, ZZAPP_END_NONE);
	life_put(ZZAPP_LIFE_STATE_OFF, ZZAPP_ST_CLAIMED);
	slot_state_lock_release();
	return AAR_OK;
}

static void put_list(uint8_t *reply, uint16_t *reply_len, uint32_t slot)
{
	zzapp_proc_t *proc = zzapp_proc_slot(slot);
	zzapp_kernel_t *kernel = zzapp_runtime_kernel();
	uint64_t cpu = 0U;
	uint64_t service = 0U;
	uint8_t block = ZZAPP_BLOCK_NONE;

	if (kernel) {
		cpu = kernel->threads[slot].user_ticks +
		      kernel->threads[slot].svc_ticks;
		service = kernel->service_ticks;
	}
	memset(reply, 0, ZZAPP_LIST_REPLY_BYTES);
	zzapp_put_be32(reply + ZZAPP_LIST_STATE, slots[slot].state);
	zzapp_put_be32(reply + ZZAPP_LIST_GENERATION, slots[slot].generation);
	if (proc)
		zzapp_put_be32(reply + ZZAPP_LIST_COMMITTED, proc->memory.committed);
	zzapp_put_be32(reply + ZZAPP_LIST_LIMIT, slots[slot].memory_limit);
	zzapp_put_be32(reply + ZZAPP_LIST_CPU_MS,
		       cpu > UINT32_MAX ? UINT32_MAX : (uint32_t)cpu);
	zzapp_put_be32(reply + ZZAPP_LIST_SERVICE_MS,
		       service > UINT32_MAX ? UINT32_MAX : (uint32_t)service);
	reply[ZZAPP_LIST_REASONS + 1U] = (uint8_t)slots[slot].terminal_reason;
	if (kernel && kernel->threads[slot].state == ZZAPP_THREAD_SLEEPING) {
		block = ZZAPP_BLOCK_SLEEP;
	} else if (kernel && kernel->threads[slot].state == ZZAPP_THREAD_BLOCKED) {
		switch (runtime_for_slot(slot)->wait_kind) {
		case ZZAPP_WAIT_FILE:
			block = ZZAPP_BLOCK_FILE;
			break;
		case ZZAPP_WAIT_CONSOLE_WRITE:
		case ZZAPP_WAIT_CONSOLE_READ:
			block = ZZAPP_BLOCK_CONSOLE;
			break;
		case ZZAPP_WAIT_DISPLAY_OPEN:
		case ZZAPP_WAIT_FLIP:
			block = ZZAPP_BLOCK_VSYNC;
			break;
		case ZZAPP_WAIT_AUDIO:
			block = ZZAPP_BLOCK_AUDIO;
			break;
		case ZZAPP_WAIT_EVENT:
			block = ZZAPP_BLOCK_EVENT;
			break;
		default:
			break;
		}
	}
	reply[ZZAPP_LIST_REASONS] = block;
	memcpy(reply + ZZAPP_LIST_NAME, slots[slot].name,
	       sizeof(slots[slot].name));
	if (kernel && kernel->overload_count != 0U)
		zzapp_put_be16(reply + ZZAPP_LIST_REASONS + 2U,
			       ZZAPP_LIST_FLAG_OVERLOAD);
	*reply_len = ZZAPP_LIST_REPLY_BYTES;
}

static void release_quarantine_locked(struct zzapp_service_slot *entry)
{
	/*
	 * Host admission remains STOPPING until this point. Publish its IDLE
	 * transition before dropping the service-side quarantine, so a claim
	 * can never observe an idle service slot with a live loader host.
	 */
	if (zzapp_host_quarantine_released)
		zzapp_host_quarantine_released(service_slot());
	entry->quarantined = 0;
	entry->quarantine_reclaimed = 0;
	entry->quarantine_release_requested = 0;
	entry->claim_started = 0U;
	entry->load_progress = 0U;
	entry->memory_limit = 0U;
	entry->host_visible_limit = 0U;
	entry->state = ZZAPP_ST_IDLE;
	life_put(ZZAPP_LIFE_STOP_OFF, 0U);
	life_put(ZZAPP_LIFE_STATE_OFF, ZZAPP_ST_IDLE);
}

static int is_load_contact(uint16_t opcode)
{
	return opcode == ZZAPP_OP_LOAD_BEGIN || opcode == ZZAPP_OP_LOAD_CHUNK ||
	       opcode == ZZAPP_OP_LOAD_END || opcode == ZZAPP_OP_START ||
	       opcode == ZZAPP_OP_STOP;
}

static int release_stale_quarantine(uint16_t opcode, const uint8_t *req,
				    int extended)
{
	uint32_t slot = 0U;

	if (!is_load_contact(opcode))
		return 0;
	if (extended) {
		if (!req)
			return 0;
		slot = zzapp_get_be32(req + ZZAPP_SEL_SLOT);
		if (slot >= ZZAPP_SLOT_COUNT)
			return 0;
		if (zzapp_get_be32(req + ZZAPP_SEL_GENERATION) ==
		    slots[slot].generation)
			return 0;
	}
	slot_state_lock_acquire();
	if (!slots[slot].quarantined) {
		slot_state_lock_release();
		return 0;
	}
	if (!slots[slot].quarantine_reclaimed)
		slots[slot].quarantine_release_requested = 1;
	if (!slots[slot].quarantine_reclaimed) {
		slot_state_lock_release();
		return 1;
	}
	select_slot(slot);
	release_quarantine_locked(&slots[slot]);
	slot_state_lock_release();
	return 1;
}

uint16_t zzapp_mailbox_dispatch(uint16_t opcode, const uint8_t *req,
				uint16_t req_len, uint8_t *reply,
				uint16_t *reply_len, int z3,
				uint32_t request_id, uint32_t user_cookie)
{
	int rc;
	int quarantine_rc;
	int extended = req_len == ZZAPP_MAILBOX_PAYLOAD_BYTES;

	if (!reply || !reply_len)
		return SDK_STATUS_BAD_REQUEST;
	*reply_len = 0U;
	if (opcode < ZZAPP_OP_FIRST || opcode > ZZAPP_OP_RANGE_HI)
		return SDK_STATUS_UNSUPPORTED;
	if (!z3) {
		if (opcode == ZZAPP_OP_QUERY)
			put_query(reply, reply_len, 0U, 0);
		return SDK_STATUS_UNSUPPORTED;
	}
	if (opcode == ZZAPP_OP_QUERY) {
		select_slot(0U);
		put_query(reply, reply_len,
			  ZZAPP_CAP_Z3 | ZZAPP_CAP_DISPLAY | ZZAPP_CAP_AUDIO |
			  ZZAPP_CAP_GL | ZZAPP_CAP_CLUT8 | ZZAPP_CAP_SLOTS |
			  (extended ? ZZAPP_CAP_LOW_MEMORY |
				      ZZAPP_CAP_LOAD_PROGRESS |
				      ZZAPP_CAP_HOST_LIMIT : 0U),
			  extended);
		return SDK_STATUS_OK;
	}
	if (opcode == ZZAPP_OP_ALLOC_WINDOW) {
		uint32_t arm = 0U;
		uint32_t host = 0U;
		uint32_t slot = 0U;

		if (extended) {
			for (slot = 0U; slot < ZZAPP_SLOT_COUNT; slot++) {
				if (slots[slot].state == ZZAPP_ST_IDLE &&
				    !slots[slot].quarantined &&
				    __atomic_load_n(&deferred[slot].state,
						    __ATOMIC_ACQUIRE) == DEFER_IDLE)
					break;
			}
			if (slot == ZZAPP_SLOT_COUNT)
				return SDK_STATUS_BUSY;
		}
		rc = claim_window(slot, &arm, &host, !extended);
		if (rc != AAR_OK)
			return zz_to_sdk(rc);
		zzapp_put_be32(reply + ZZAPP_ALLOC_ARM_ADDRESS, arm);
		zzapp_put_be32(reply + ZZAPP_ALLOC_SIZE, ZZAPP_WINDOW_BYTES);
		zzapp_put_be32(reply + ZZAPP_ALLOC_HOST_OFFSET, host);
		if (extended) {
			zzapp_put_be32(reply + ZZAPP_ALLOC_SLOT, slot);
			zzapp_put_be32(reply + ZZAPP_ALLOC_GENERATION,
				       slots[slot].generation);
			*reply_len = ZZAPP_ALLOC_EXT_REPLY_BYTES;
		} else {
			*reply_len = ZZAPP_ALLOC_REPLY_BYTES;
		}
		return SDK_STATUS_OK;
	}
	if (opcode == ZZAPP_OP_LIST) {
		uint32_t slot;

		if (!extended || !req)
			return SDK_STATUS_BAD_REQUEST;
		slot = zzapp_get_be32(req + ZZAPP_SEL_SLOT);
		if (slot >= ZZAPP_SLOT_COUNT)
			return SDK_STATUS_BAD_HANDLE;
		put_list(reply, reply_len, slot);
		return SDK_STATUS_OK;
	}
	rc = select_request_slot(req, req_len, extended);
	if (opcode == ZZAPP_OP_RELEASE) {
		uint32_t slot;

		if (!extended || !req)
			return SDK_STATUS_BAD_REQUEST;
		slot = zzapp_get_be32(req + ZZAPP_SEL_SLOT);
		if (slot >= ZZAPP_SLOT_COUNT)
			return SDK_STATUS_BAD_HANDLE;
		slot_state_lock_acquire();
		if (!slots[slot].quarantined ||
		    !slots[slot].quarantine_reclaimed ||
		    zzapp_get_be32(req + ZZAPP_SEL_GENERATION) !=
			    slots[slot].generation) {
			slot_state_lock_release();
			return SDK_STATUS_BUSY;
		}
		select_slot(slot);
		release_quarantine_locked(&slots[slot]);
		slot_state_lock_release();
		return SDK_STATUS_OK;
	}
	quarantine_rc = release_stale_quarantine(opcode, req, extended);
	if (quarantine_rc)
		return quarantine_rc > 0 ? SDK_STATUS_BAD_HANDLE : SDK_STATUS_BUSY;
	if (rc != SDK_STATUS_OK)
		return (uint16_t)rc;
	switch (opcode) {
	case ZZAPP_OP_STOP:
		if (!extended && req_len >= ZZAPP_STOP_REQ_BYTES) {
			uint32_t expected;

			if (!req)
				return SDK_STATUS_BAD_REQUEST;
			expected = zzapp_get_be32(req + ZZAPP_STOP_GENERATION);
			if (expected != zzapp_services_generation())
				return SDK_STATUS_BAD_HANDLE;
		}
		slot_state_lock_acquire();
		if (slots[service_slot()].state == ZZAPP_ST_CLAIMED) {
			int release_now = stop_current_deferred();

			if (release_now == 0) {
				begin_claim_cancellation(&slots[service_slot()],
							 ZZAPP_END_FORCED);
				slot_state_lock_release();
				return SDK_STATUS_OK;
			}
			if (release_now > 0) {
				memset(current_runtime(), 0, sizeof(*current_runtime()));
				slots[service_slot()].memory_limit = 0U;
				slots[service_slot()].host_visible_limit = 0U;
				slots[service_slot()].terminal_reason = ZZAPP_END_FORCED;
				publish_claim_idle(&slots[service_slot()]);
				life_put(ZZAPP_LIFE_TERMINAL_OFF, ZZAPP_END_FORCED);
				slot_state_lock_release();
				return SDK_STATUS_OK;
			}
		}
		zzapp_host_latch_stop(service_slot());
		life_put(ZZAPP_LIFE_STOP_OFF, 1U);
		slot_state_lock_release();
		return SDK_STATUS_OK;
	case ZZAPP_OP_STATUS:
		zzapp_put_be32(reply + ZZAPP_STATUS_STATE, zzapp_services_state());
		zzapp_put_be32(reply + ZZAPP_STATUS_GENERATION,
			       zzapp_services_generation());
		zzapp_put_be32(reply + ZZAPP_STATUS_FAULT, life_get(ZZAPP_LIFE_FAULT_OFF));
		zzapp_put_be32(reply + ZZAPP_STATUS_EXIT, life_get(ZZAPP_LIFE_EXIT_OFF));
		zzapp_put_be32(reply + ZZAPP_STATUS_FLAGS, zzapp_services_flags());
		zzapp_put_be32(reply + ZZAPP_STATUS_PC, life_get(ZZAPP_LIFE_PC_OFF));
		zzapp_put_be32(reply + ZZAPP_STATUS_SP, life_get(ZZAPP_LIFE_SP_OFF));
		zzapp_put_be32(reply + ZZAPP_STATUS_LR, life_get(ZZAPP_LIFE_LR_OFF));
		zzapp_put_be32(reply + ZZAPP_STATUS_FAR, life_get(ZZAPP_LIFE_FAR_OFF));
		zzapp_put_be32(reply + ZZAPP_STATUS_FSR, life_get(ZZAPP_LIFE_FSR_OFF));
		zzapp_put_be32(reply + ZZAPP_STATUS_CPSR, life_get(ZZAPP_LIFE_CPSR_OFF));
		zzapp_put_be32(reply + ZZAPP_STATUS_FLIP_GEN,
			       life_get(ZZAPP_LIFE_FLIP_GEN_OFF));
		*reply_len = ZZAPP_STATUS_REPLY_BYTES;
		zzapp_status_observed();
		return SDK_STATUS_OK;
	case ZZAPP_OP_DISPLAY_BIND:
		rc = bind_surface(req, req_len);
		if (zz_to_sdk(rc) == SDK_STATUS_BAD_REQUEST)
			report_rejected_app_op(opcode, "display bind fields rejected");
		return zz_to_sdk(rc);
	case ZZAPP_OP_LOAD_BEGIN:
		if (extended) {
			uint32_t mib = zzapp_get_be32(req + ZZAPP_LOAD_MEM_LIMIT_MIB);
			uint32_t host_mib =
				zzapp_get_be32(req + ZZAPP_LOAD_HOST_VISIBLE_MIB);

			if (mib > UINT32_MAX / (1024U * 1024U) ||
			    host_mib > UINT32_MAX / (1024U * 1024U))
			{
				report_rejected_app_op(opcode, "load limit above 4095 MiB");
				return SDK_STATUS_BAD_REQUEST;
			}
			/* The limit is a cap, not a reservation: only a request
			 * the heap and the app's alias range could never hold is
			 * refused. Allocations can still fail below the cap. */
			if (host_mib > zzapp_host_visible_capacity() /
				       (1024U * 1024U))
			{
				report_rejected_app_op(opcode,
					"host-visible limit above the card's ceiling");
				return SDK_STATUS_NO_MEMORY;
			}
		}
		/* fall through */
	case ZZAPP_OP_LOAD_CHUNK:
	case ZZAPP_OP_LOAD_END:
	case ZZAPP_OP_START:
		if (request_id == 0U) {
			report_rejected_app_op(opcode, "deferred operation has request id zero");
			return SDK_STATUS_BAD_REQUEST;
		}
		if (!scheduler_core1_available())
			return SDK_STATUS_UNSUPPORTED;
		if (!queue_deferred(opcode, req, req_len, request_id, user_cookie))
			return SDK_STATUS_BUSY;
		return SDK_STATUS_QUEUED;
	default:
		return SDK_STATUS_UNSUPPORTED;
	}
}

int zzapp_mailbox_harvest(uint32_t *request_id, uint32_t *user_cookie,
			  uint16_t *opcode, uint16_t *status,
			  uint8_t *payload, uint16_t *payload_len)
{
	struct zzapp_deferred *defer;
	uint32_t slot;

	slot_state_lock_acquire();
	if (deferred_harvest_slot == ZZAPP_SLOT_COUNT) {
		for (slot = 0U; slot < ZZAPP_SLOT_COUNT; slot++) {
			if (__atomic_load_n(&deferred[slot].state, __ATOMIC_ACQUIRE) ==
			    DEFER_DONE)
				break;
		}
		if (slot == ZZAPP_SLOT_COUNT) {
			slot_state_lock_release();
			return 0;
		}
		deferred_harvest_slot = slot;
	}
	defer = &deferred[deferred_harvest_slot];
	if (request_id)
		*request_id = defer->request_id;
	if (user_cookie)
		*user_cookie = defer->cookie;
	if (opcode)
		*opcode = defer->opcode;
	if (status)
		*status = defer->status;
	if (payload_len)
		*payload_len = defer->reply_len;
	if (payload && defer->reply_len != 0U)
		memcpy(payload, defer->reply, defer->reply_len);
	slot_state_lock_release();
	return 1;
}

void zzapp_mailbox_harvest_ack(void)
{
	slot_state_lock_acquire();
	if (deferred_harvest_slot != ZZAPP_SLOT_COUNT &&
	    deferred[deferred_harvest_slot].state == DEFER_DONE)
		deferred[deferred_harvest_slot].state = DEFER_IDLE;
	deferred_harvest_slot = ZZAPP_SLOT_COUNT;
	slot_state_lock_release();
}

#ifdef ZZAPP_HOST_TEST
void zzapp_services_test_reset(void)
{
	uint32_t slot;

	for (slot = 0U; slot < ZZAPP_SLOT_COUNT; slot++) {
		select_slot(slot);
		audio_detach();
	}
	memset(runtime, 0, sizeof(runtime));
	memset(test_window_backing, 0, sizeof(test_window_backing));
	memset(test_window_range_held, 0, sizeof(test_window_range_held));
	memset(test_window_serial, 0, sizeof(test_window_serial));
	memset(slots, 0, sizeof(slots));
	memset(deferred, 0, sizeof(deferred));
	selected_slot[0] = 0U;
	selected_slot[1] = 0U;
	deferred_harvest_slot = ZZAPP_SLOT_COUNT;
	deferred_poll_next_slot = 0U;
	display_mailbox_slot = -1;
	defer_test_core = 1;
	yield_hook = NULL;
	time_hook = NULL;
#ifdef ZZAPP_SERVICES_STANDALONE_TEST
	service_test_current_thread = -1;
	service_test_wake_count = 0U;
	service_loader_generation = 0U;
	service_test_loader_set_generation_hook = NULL;
	service_test_grant_count = 0U;
	service_test_ungrant_count = 0U;
	service_test_loader_begin_hook = NULL;
	service_test_loader_chunk_hook = NULL;
	service_test_loader_end_hook = NULL;
	service_test_deferred_completion_hook = NULL;
	service_test_deferred_completing_hook = NULL;
	service_test_deferred_reply_hook = NULL;
	service_test_loader_abort_count = 0U;
#endif
	memset(slot_busy_test, 0, sizeof(slot_busy_test));
	zzapp_services_on_launch();
}

void zzapp_services_test_set_core(int core)
{
	defer_test_core = core;
}

void zzapp_services_test_set_window(void *base, uint32_t bytes)
{
	zzapp_services_test_set_window_slot(0U, base, bytes);
}

void zzapp_services_test_set_window_slot(uint32_t slot, void *base, uint32_t bytes)
{
	if (slot >= ZZAPP_SLOT_COUNT)
		return;
	select_slot(slot);
	if (base && bytes >= ZZAPP_WINDOW_BYTES) {
		uint32_t serial = 0U;

		if (zzapp_test_card_alloc_at)
			(void)zzapp_test_card_alloc_at(
				ZZAPP_WINDOW_Z3_SLOT_ADDRESS(slot),
				ZZAPP_WINDOW_BYTES, &serial);
		test_window_serial[slot] = serial;
		test_window_range_held[slot] = 1;
		init_window((uint8_t *)base, bytes, 1);
	}
}

/* Lets a claim allocate this slot's window again once a reset has released
 * it, like the allocator does for the slot's fixed range. Without it a reset
 * leaves the slot without a window until core 0 binds one. */
void zzapp_services_test_set_window_pool(uint32_t slot, void *base)
{
	if (slot < ZZAPP_SLOT_COUNT)
		test_window_backing[slot] = (uint8_t *)base;
}

void zzapp_services_test_set_yield(void (*fn)(void))
{
	yield_hook = fn;
}

void zzapp_services_test_set_time(uint64_t (*fn)(void))
{
	time_hook = fn;
}

void zzapp_services_test_set_slot_busy(uint32_t slot, int busy)
{
	if (slot < 4U)
		slot_busy_test[slot] = busy ? 1 : 0;
}

void zzapp_services_test_set_audio_request(uint32_t request)
{
	current_runtime()->audio_request = request;
}

uint32_t zzapp_services_test_audio_request(void)
{
	return current_runtime()->audio_request;
}

uint32_t zzapp_services_test_audio_detach_count(void)
{
	return current_runtime()->audio_detach_count;
}
#endif
