/*
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 *
 * The app runtime's services over the AAR transport: claims and their
 * control blocks, the deferred load mailbox ops, files, console, log, events
 * and messages, display, palette and audio, all as requests, completions and
 * messages on the claim's queue pair (zzapp_xport.[ch]), and the quarantine
 * of claims whose client is gone.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "zzapp_services.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "aar/app.h"
#include "aar/detail/present_feedback.h"
#include "aar/transport.h"
#include "zzapp_proto.h"
#include "zzapp_runtime.h"
#include "zzapp_rec.h"
#include "zzapp_svc.h"
#include "zzapp_xport.h"
void zzapp_host_yield_wait(void);
void __attribute__((weak)) zzapp_host_latch_stop(uint32_t slot)
{
	(void)slot;
}
/* Largest host-visible limit a client may request. The host runtime
 * defines it; a build that links only the services gets no ceiling. */
uint32_t __attribute__((weak)) zzapp_host_visible_capacity(void)
{
	return UINT32_MAX;
}
/* The groups the host runtime serves. A build that links only the services
 * has no table, so every group a client can use counts as present. */
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
/* Returns the host-visible blocks of the selected record; the quarantine
 * holds them back until the shell is released. */
extern void zzapp_host_revoke_visible(void) __attribute__((weak));

extern uint32_t zzapp_loader_pool_free(void) __attribute__((weak));
int zzapp_rec_retire(uint32_t pid);
#ifdef ZZAPP_HOST_TEST
/* Host recorder for the present-path flush: production code decides what
 * to clean; suites assert the presented card range and that the size is
 * the surface, never more. */
struct zzapp_present_flush_record {
	unsigned long card;
	unsigned long bytes;
};
struct zzapp_present_flush_record zzapp_test_present_flushes[32];
unsigned zzapp_test_present_flush_count;
int zzapp_test_display_dma_busy;
unsigned zzapp_test_display_invalidate_count;
uint32_t zzapp_test_display_invalidate_last_address;
uint32_t zzapp_test_display_invalidate_last_bytes;
__attribute__((weak)) int scheduler_core1_available(void) { return 1; }
__attribute__((weak)) int zzapp_minigl_swap_tracked(uint32_t id, uint64_t frame_id)
{
	(void)id;
	(void)frame_id;
	return AAR_E_NOSYS;
}
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
#include "zzm_minigl_host.h"
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

/* Requests carry paths and names as bytes in the scratch area. */
#define ZZAPP_SVC_PATH_MAX    255u
#define ZZAPP_SVC_NAME_ROOM   256u
/* Every AAR_DISPLAY_* flag of API 0.6: VSYNC, LATEST, WINDOW, RESIZABLE,
 * POINTER_ABS, TEXT_INPUT, CLOSE_REQUEST, SCALE. */
#define ZZAPP_DISPLAY_FLAGS_ALL 0xffu
/* The runtime keeps a vertical-blank wait this long past the app's timeout:
 * the client ends it with AAR_E_TIMEDOUT at the timeout (rounded up to its
 * clock), and a request the runtime gave up on a moment earlier would leave
 * the client's watch armed for the app's next call. The wait is never
 * shorter than the timeout the app asked for. */
#define ZZAPP_VBLANK_GRACE_US 20000ull
/* File descriptors the services hand out for the AAR file and list calls. */
#ifndef ZZAPP_FD_BASE
#define ZZAPP_FD_BASE         3
#endif
#ifndef ZZAPP_FD_COUNT
#define ZZAPP_FD_COUNT        16
#endif

#ifndef ZZAPP_HOST_TEST
/* The host-visible heap of the card as the transport's allocator. */
static int card_heap_alloc(void *ctx, uint32_t bytes, uint32_t align,
			   uint32_t lo, uint32_t hi, void **ptr, uint32_t *card)
{
	uint32_t addr;

	(void)ctx;
	addr = (lo != 0U || hi != 0U) ?
		surface_allocator_alloc_range(bytes, align, lo, hi) :
		surface_allocator_alloc_aligned(bytes, align);
	if (addr == 0U)
		return -1;
	*ptr = (void *)(uintptr_t)addr;
	*card = addr;
	return 0;
}

static int card_heap_release(void *ctx, void *ptr, uint32_t card)
{
	(void)ctx;
	(void)ptr;
	return surface_allocator_free(card) == 0 ? 0 : -1;
}
#endif
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

/* The transport takes the card's allocator, barriers and token base the first
 * time a claim needs it. A host test installs its own with zzapp_xport_init
 * before it claims. */
static void xport_env_ensure(void)
{
#ifndef ZZAPP_HOST_TEST
	zzapp_xport_env_t env;

	if (zzapp_xport_env()->alloc)
		return;
	memset(&env, 0, sizeof(env));
	env.alloc = card_heap_alloc;
	env.release = card_heap_release;
	/* The pairs and buffers stay out of the non-cacheable SDK pages. */
	env.lo = ZZAPP_HEAP_LO;
	env.hi = ZZAPP_UNCACHED_LO;
	env.token_adjust = ADDR_ADJ;
	env.acquire = zzapp_xport_dmb;
	env.release_barrier = zzapp_xport_dmb;
	zzapp_xport_init(&env);
#endif
}

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

/* What an app is parked on. A request wait (FILE, CONSOLE_READ,
 * DISPLAY_OPEN, AUDIO_REQ) has the id of the blocking request in
 * wait_value; the name only says what LIST reports. */
enum zzapp_wait_kind {
	ZZAPP_WAIT_NONE,
	ZZAPP_WAIT_FILE,
	ZZAPP_WAIT_CONSOLE_WRITE,
	ZZAPP_WAIT_CONSOLE_READ,
	ZZAPP_WAIT_DISPLAY_OPEN,
	ZZAPP_WAIT_FLIP,
	ZZAPP_WAIT_AUDIO,
	ZZAPP_WAIT_EVENT,
	ZZAPP_WAIT_SQ_SPACE,
	ZZAPP_WAIT_AUDIO_REQ
};
#if ZZAPP_PERF_COUNTERS
/* The scheduling counters name the park cause by this value (zzapp_kernel.c,
 * zzapp_kernel_sched_report). */
_Static_assert(ZZAPP_WAIT_AUDIO_REQ < ZZAPP_KERNEL_PARK_SLEEP,
	       "wait kinds fit the park causes");
#endif

/* The one mailbox op in flight per record (LOAD_BEGIN, LOAD_CHUNK, LOAD_END,
 * START); its reply is harvested by core 0. */
struct zzapp_deferred {
	volatile uint32_t state;
	uint16_t opcode;
	uint16_t status;
	uint16_t req_len;
	uint16_t reply_len;
	uint32_t request_id;
	uint32_t cookie;
	volatile uint32_t cancelled;
	uint8_t req[16];
	uint8_t reply[16];
	volatile uint32_t start_pending;
	uint32_t start_argc;
};

/* Core 1 only. The service pass can retire a posted request, but only the
 * calling app's context accesses its private buffer. */
struct zzapp_file_io {
	void *buf;
	uint32_t ticket, serial;
	uint32_t len, count, chunk;
	int fd;
	int32_t result;
	uint16_t op;
	uint8_t finished;
};

/* The services' per-record state, in the services part of the record. */
struct zzapp_service_runtime {
	struct zzapp_deferred defer;
	int file_open[ZZAPP_FD_COUNT];
	/* AAR_FILE_READ and AAR_FILE_WRITE of the open that made the id: a
	 * read through an id opened without READ, or a write through one
	 * opened without WRITE, is AAR_E_BADF (spec 7.1). */
	uint8_t file_access[ZZAPP_FD_COUNT];
	struct zzapp_file_io file_io;
	int list_open[ZZAPP_FD_COUNT];
	volatile int display_bound;
	int display_back_owned;
	uint32_t display_id;          /* the surface id the app sees */
	uint32_t display_client_id;   /* the id the client gave its screen */
	uint32_t display_req_w;
	uint32_t display_req_h;
	uint32_t display_req_format;
	uint32_t display_width;
	uint32_t display_height;
	uint32_t display_pitch;
	uint32_t display_format;
	uint32_t display_layout;
	uint32_t display_bytes;
	uint32_t display_front;
	uint32_t display_back;
	uint32_t display_spare;
	uint32_t display_alias_phys[3];
	uint32_t display_alias[3];
	/* The flip handshake: core 1 posts and completes, core 0's vblank ISR
	 * acknowledges. Written under g_slot_state_lock. */
	volatile uint32_t display_flip_gen;
	volatile uint32_t display_flip_ack;
	volatile uint32_t display_flip_submit;
	volatile uint32_t display_flip_address;
	/* The completion's result of the flip in flight, 0 until it is in: a
	 * failed flip leaves the scanout alone, so no page is handed over. */
	volatile int32_t display_flip_result;
	/* When the flip was posted, or when the client submitted the scanout. */
	uint64_t display_flip_submit_us;
	/* Client flip ticket, under g_slot_state_lock. */
	uint64_t display_flip_frame;
	uint32_t display_reply_expired;
	int display_wait_feedback;
	/* The aar_time_us value at which the last flip or GL swap was seen
	 * shown, 0 before the first. Written and read under g_slot_state_lock
	 * (a 64-bit word is not atomic on this core). */
	uint64_t display_present_us;
	/* AAR_DISPLAY_* the display was opened with, and the feature caps the
	 * client granted (AAR_DISPCAP_WINDOW .. SCALED, bits 3 to 8). */
	uint32_t display_flags;
	uint32_t display_caps;
	/* The P96 screen a WINDOW or SCALED display's client named at open
	 * (AAR_DISP_REPLY_SCANOUT), as a card address, 0 when none or not valid.
	 * Only an address to compare the ISR's scanout address with: never
	 * leased, mapped or published as an app surface. Written under
	 * g_slot_state_lock. */
	uint32_t display_source;
	/* The native vertical-blank wait of a direct-scanout display: the count
	 * of blanks the card has seen on this record's screen (written under
	 * g_slot_state_lock by core 0's blank callbacks, only while a waiter
	 * is registered), and the waiter's registration. */
	volatile uint32_t vblank_seq;
	volatile uint32_t vblank_waiting;
	uint32_t display_ack_seen;
	uint32_t display_queued;
	uint32_t display_scanout_ack;
	uint32_t display_mailbox_replaced;
	volatile int display_mailbox_active;
	int display_mailbox_gl;
	uint32_t display_mailbox_shown;
	uint32_t display_mailbox_pending;
	/* Changes hands with display_mailbox_pending, under its lock. */
	uint64_t display_mailbox_frame;
	uint64_t display_mailbox_started_us;
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
	volatile uint32_t audio_first_write_seen;
	uint32_t audio_grant_base;
	uint32_t audio_grant_bytes;
	uint32_t audio_alias;
	volatile uint32_t audio_request;
	int audio_result;
	int audio_teardown;
	int audio_ahi_route;
	uint8_t name[ZZAPP_LIST_NAME_BYTES];
#if ZZAPP_PERF_COUNTERS
	uint8_t audio_low_water_latched;
#endif
	enum zzapp_wait_kind wait_kind;
	uint32_t wait_value;
	uint64_t wait_deadline;
	/* The console output stream: free-running indices into the ring of the
	 * service block, and the bytes of the one CONSOLE_WRITE in flight. */
	uint32_t console_head;
	uint32_t console_tail;
	uint32_t console_inflight;
	/* The claim's lease: when it was last renewed, the client's lease counter
	 * as last seen, and the last load progress. */
	uint64_t claim_started;
	uint64_t load_progress;
	uint32_t lease_seen;
	/* 1 once the quarantine revoked the handle. */
	uint32_t handle_revoked;
	/* Palette entries written since the last palette request went out
	 * (bit n: entry n); the colours themselves are in the shadow of the
	 * service block's palette area. */
	uint32_t palette_dirty[ZZAPP_XPORT_PALETTE_ENTRIES / 32U];
	/* Palette entries the app has set since the display opened (bit n:
	 * entry n), so a resize that replaces the client's screen can send the
	 * colours again. */
	uint32_t palette_set[ZZAPP_XPORT_PALETTE_ENTRIES / 32U];
	/* App-scoped, under g_slot_state_lock. A submission can park before
	 * committing, so a stop must not publish its tentative reservation. */
	aar_present_queue present_feedback;
	int present_submitting;
#ifdef ZZAPP_HOST_TEST
	uint32_t audio_detach_count;
#endif
};
_Static_assert(sizeof(struct zzapp_service_runtime) <= ZZAPP_REC_SERVICES_BYTES,
	       "service runtime does not fit its record block");

/*
 * Records with something for a pass to do. A record is in a set from the
 * moment it may need the pass until a pass finds it has nothing left, so the
 * passes cost what the busy records cost, not what the capacity costs
 * (design 5.1). Every use holds g_slot_state_lock.
 *   set_svc      core 1: a parked wait, a doorbell, console output in flight
 *   set_core0    core 0: an audio stream or request, a deferred op, a claim
 *                that is loading (lease), a reply to harvest
 *   set_display  the vblank ISR: a bound display
 */
struct pid_set {
	uint32_t count;
	uint16_t pids[ZZAPP_REC_MAX_APPS];
	uint16_t pos[ZZAPP_REC_MAX_APPS]; /* index + 1; 0: not in the set */
};

static struct pid_set set_svc;
static struct pid_set set_core0;
static struct pid_set set_display;

static void set_add(struct pid_set *set, uint32_t pid)
{
	if (pid >= ZZAPP_REC_MAX_APPS || set->pos[pid] != 0U)
		return;
	set->pids[set->count] = (uint16_t)pid;
	set->count++;
	set->pos[pid] = (uint16_t)set->count;
}

/* Swaps the last entry into the hole, so a pass walking down by index may
 * delete the entry it stands on. */
static void set_del(struct pid_set *set, uint32_t pid)
{
	uint32_t at;
	uint32_t last;

	if (pid >= ZZAPP_REC_MAX_APPS || set->pos[pid] == 0U)
		return;
	at = (uint32_t)set->pos[pid] - 1U;
	last = set->pids[set->count - 1U];
	set->pids[at] = (uint16_t)last;
	set->pos[last] = (uint16_t)(at + 1U);
	set->pos[pid] = 0U;
	set->count--;
}

#ifdef ZZAPP_HOST_TEST
static void set_clear(struct pid_set *set)
{
	memset(set, 0, sizeof(*set));
}
#endif

static uint32_t ctx_pid[2] = { ZZAPP_PID_NONE, ZZAPP_PID_NONE };
static uint32_t deferred_harvest_pid = ZZAPP_PID_NONE;
static uint32_t deferred_poll_cursor;
static struct zzapp_service_runtime *display_mailbox_state;
static uint32_t display_mailbox_pid = ZZAPP_PID_NONE;
static int slot_busy_test[4];
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
static int audio_handoff_pending(const struct zzapp_service_runtime *state);
static int console_pending(zzapp_rec_t *rec);
static void finish_shell_release(zzapp_rec_t *rec);
static int discard_deferred(zzapp_rec_t *rec);
static void cbf_or_current(uint32_t bits);
static int blocking_request(uint32_t op, enum zzapp_wait_kind wk,
			    uint64_t deadline, const uint32_t *args,
			    uint32_t nargs, const zzapp_xport_buf_t *buf,
			    uint32_t buf_len, aar_completion *out);
static int req_post(zzapp_xport_req_kind_t kind, const aar_request *req,
		    uint64_t deadline, uint32_t *id);

/* The per-core selected context: the record the services act for. */
struct svc_ctx {
	uint32_t pid;
};

static struct svc_ctx ctx_save(void)
{
	struct svc_ctx saved = { ctx_pid[service_context()] };

	return saved;
}

static void ctx_restore(struct svc_ctx saved)
{
	ctx_pid[service_context()] = saved.pid;
}

static struct zzapp_service_runtime *runtime_of(zzapp_rec_t *rec)
{
	return (struct zzapp_service_runtime *)(void *)rec->services.bytes;
}

/* The record the services act for: the running app's own on core 1, else the
 * selected one. ZZAPP_PID_NONE when the context has no record. */
static uint32_t service_pid(void)
{
	int thread = zzapp_kernel_current_thread();

	if (service_context() == 1U && thread >= 0)
		return (uint32_t)thread;
	return ctx_pid[service_context()];
}

static zzapp_rec_t *service_rec(void)
{
	uint32_t pid = service_pid();

	if (pid == ZZAPP_PID_NONE || zzapp_rec_state(pid) == ZZAPP_REC_FREE)
		return NULL;
	return zzapp_rec_by_pid(pid);
}

static zzapp_proc_t *service_proc(void)
{
	zzapp_rec_t *rec = service_rec();

	return rec ? &rec->proc : NULL;
}

static struct zzapp_service_runtime *current_runtime(void)
{
	zzapp_rec_t *rec = service_rec();

	return rec ? runtime_of(rec) : NULL;
}

/*
 * Record lifetime. A record is type stable (its bytes stay readable once it
 * is FREE) and its transport blocks are freed only under g_slot_state_lock
 * (release_shell_locked), after it left every set. A pass that walks a set
 * therefore reads and uses a record in the same lock section as the lookup;
 * a pass that must not hold the lock for its whole work (a ring copy, an
 * allocation) reads what it needs under the lock and drops it only when the
 * record cannot be freed behind it: an open audio ring or a pending audio
 * request keeps the shell until the handoff completes (audio_handoff_pending),
 * and the display mailbox is disabled under its own lock before the free
 * (display_mailbox_disable).
 */
#ifdef ZZAPP_SERVICES_STANDALONE_TEST
/* Runs inside a pass after its lookup, before the record is used: where the
 * other core's retirement would land if the pass did not hold the lock. */
static void (*service_test_lookup_hook)(uint32_t pid);
#define SERVICE_LOOKUP_GAP(pid) \
	do { \
		if (service_test_lookup_hook) \
			service_test_lookup_hook(pid); \
	} while (0)
#else
#define SERVICE_LOOKUP_GAP(pid) do { } while (0)
#endif

static int wait_status(void)
{
	int status = zzapp_kernel_wait_status(zzapp_runtime_kernel(), service_pid());

	if (status == ZZAPP_KERNEL_WAIT_TIMEOUT)
		return AAR_E_TIMEDOUT;
	if (status == ZZAPP_KERNEL_WAIT_CANCELLED)
		return AAR_E_IO;
	return AAR_OK;
}

/* Asks core 1 for a service pass: the doorbell SGI interrupts the running app
 * and the pass runs at once, where waiting for the next 5 ms tick would delay
 * every parked app behind whatever else holds the core. Whoever makes a
 * parked wait ready without the client's KICK rings it. Host builds count the
 * rings. */
#ifdef ZZAPP_HOST_TEST
static uint32_t core1_doorbells;

uint32_t zzapp_services_test_core1_doorbells(void)
{
	return core1_doorbells;
}
#endif

static void ring_core1(void)
{
#ifdef ZZAPP_HOST_TEST
	core1_doorbells++;
#else
	scheduler_core1_publish();
#endif
}

/* Registers the wait with the system pass: the record joins the set the pass
 * walks, and stays in it until its wait and its console output are done. */
static void wait_begin(enum zzapp_wait_kind kind, uint32_t value,
		       uint64_t deadline)
{
	struct zzapp_service_runtime *state = current_runtime();

	if (!state)
		return;
	state->wait_kind = kind;
	state->wait_value = value;
	state->wait_deadline = deadline;
	/* The scheduling counters split the park by this kind. */
	zzapp_kernel_park_kind(zzapp_runtime_kernel(), service_pid(),
			       (uint32_t)kind);
	slot_state_lock_acquire();
	set_add(&set_svc, service_pid());
	slot_state_lock_release();
}

static void wait_end(void)
{
	struct zzapp_service_runtime *state = current_runtime();

	if (!state)
		return;
	state->wait_kind = ZZAPP_WAIT_NONE;
	state->wait_value = 0U;
	state->wait_deadline = 0U;
	zzapp_kernel_park_kind(zzapp_runtime_kernel(), service_pid(), 0U);
}

int zzapp_services_wait_registered(uint32_t pid)
{
	zzapp_rec_t *rec = zzapp_rec_by_pid(pid);

	return rec && zzapp_rec_state(pid) != ZZAPP_REC_FREE &&
	       runtime_of(rec)->wait_kind != ZZAPP_WAIT_NONE;
}



#ifdef ZZAPP_SERVICES_STANDALONE_TEST
static unsigned service_test_wake_count;
#endif
static uint32_t service_stub_direct_audio;

/*
 * Kernel hooks. Weak, so the kernel overrides them; a build that links only
 * the services gets these. The thread and the per-record kernel words are
 * the record's own (rec->thread, rec->kernel), so they need no kernel table.
 */
__attribute__((weak)) void zzapp_kernel_reset_cpu_ticks(
	zzapp_kernel_t *kernel, uint32_t pid)
{
	zzapp_rec_t *rec = zzapp_rec_by_pid(pid);

	(void)kernel;
	if (!rec)
		return;
	rec->thread.user_ticks = 0U;
	rec->thread.svc_ticks = 0U;
}

#if ZZAPP_PERF_COUNTERS
__attribute__((weak)) void zzapp_kernel_perf_audio(zzapp_kernel_t *kernel,
						     uint32_t pid,
						     uint32_t low_water,
						     uint32_t starved,
						     uint32_t underruns)
{
	(void)kernel;
	(void)pid;
	(void)low_water;
	(void)starved;
	(void)underruns;
}
#endif

__attribute__((weak)) int zzapp_kernel_audio_open(zzapp_kernel_t *kernel,
						    uint32_t pid, int direct)
{
	zzapp_rec_t *rec = zzapp_rec_by_pid(pid);

	(void)kernel;
	if (!rec || (direct && service_stub_direct_audio >= 2U))
		return -1;
	if (rec->kernel.resources.direct_audio)
		return -1;
	rec->kernel.resources.direct_audio = direct ? 1U : 2U;
	if (direct)
		service_stub_direct_audio++;
	return 0;
}

__attribute__((weak)) void zzapp_kernel_audio_close(zzapp_kernel_t *kernel,
						      uint32_t pid)
{
	zzapp_rec_t *rec = zzapp_rec_by_pid(pid);

	(void)kernel;
	if (!rec)
		return;
	if (rec->kernel.resources.direct_audio == 1U &&
	    service_stub_direct_audio != 0U)
		service_stub_direct_audio--;
	rec->kernel.resources.direct_audio = 0U;
}

__attribute__((weak)) int zzapp_kernel_wait_status(
	const zzapp_kernel_t *kernel, uint32_t pid)
{
	zzapp_rec_t *rec = zzapp_rec_by_pid(pid);

	(void)kernel;
	if (!rec)
		return ZZAPP_KERNEL_WAIT_CANCELLED;
	if (rec->thread.wake_reason == ZZAPP_WAKE_TIMEOUT)
		return ZZAPP_KERNEL_WAIT_TIMEOUT;
	if (rec->thread.wake_reason == ZZAPP_WAKE_CANCELLED)
		return ZZAPP_KERNEL_WAIT_CANCELLED;
	return 0;
}

__attribute__((weak)) int zzapp_kernel_wake(zzapp_kernel_t *kernel,
					      uint32_t pid,
					      zzapp_wake_reason_t reason)
{
	zzapp_rec_t *rec = zzapp_rec_by_pid(pid);

	(void)kernel;
	if (!rec)
		return -1;
#ifdef ZZAPP_SERVICES_STANDALONE_TEST
	service_test_wake_count++;
#endif
	rec->thread.state = ZZAPP_THREAD_RUNNABLE;
	rec->thread.wake_reason = reason;
	return 0;
}

__attribute__((weak)) void zzapp_kernel_park_kind(zzapp_kernel_t *kernel,
						  uint32_t pid, uint32_t kind)
{
	(void)kernel;
	(void)pid;
	(void)kind;
}

__attribute__((weak)) int zzapp_kernel_surface_bind(zzapp_kernel_t *kernel,
						      uint32_t pid,
						      uint32_t generation,
						      uint32_t base,
						      uint32_t bytes)
{
	(void)kernel;
	(void)pid;
	(void)generation;
	return base != 0U && bytes != 0U ? 0 : -1;
}
__attribute__((weak)) void zzapp_kernel_surface_unbind(
	zzapp_kernel_t *kernel, uint32_t pid)
{
	(void)kernel;
	(void)pid;
}

#ifndef ZZAPP_SERVICES_STANDALONE_TEST
static zzapp_kernel_t service_fallback_kernel;

__attribute__((weak)) int zzapp_kernel_current_thread(void)
{
	return -1;
}

__attribute__((weak)) zzapp_kernel_t *zzapp_runtime_kernel(void)
{
	return &service_fallback_kernel;
}
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
	uint32_t pid;

	/* Core 1 can hold any record's mailbox lock, not only the selected one. */
	for (pid = 0U; pid < zzapp_rec_capacity(); pid++) {
		zzapp_rec_t *rec = zzapp_rec_by_pid(pid);

		if (!rec)
			continue;
#ifndef ZZAPP_HOST_TEST
		sdk_smp_lock_reset(&runtime_of(rec)->display_mailbox_lock);
#else
		__atomic_clear(&runtime_of(rec)->display_mailbox_lock,
			       __ATOMIC_RELEASE);
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
	current_runtime()->display_mailbox_frame = 0U;
	current_runtime()->display_mailbox_latch_pending = 0;
	display_mailbox_lock_release();
}

static uint32_t defer_state_load(const struct zzapp_deferred *defer)
{
	return __atomic_load_n(&defer->state, __ATOMIC_ACQUIRE);
}

static int defer_cancelled(const struct zzapp_deferred *defer)
{
	return __atomic_load_n(&defer->cancelled, __ATOMIC_ACQUIRE) != 0U;
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

/* Selects the record `pid` as this core's context: 1 when it is selected. A
 * pid outside the table or a FREE record clears the record context and
 * returns 0, so a caller that read the record's state a moment ago, with
 * the other core free to free it since, learns here that it is gone. */
int zzapp_services_select_pid(uint32_t pid)
{
	zzapp_rec_t *rec = zzapp_rec_by_pid(pid);
	unsigned core = service_context();

	if (!rec || zzapp_rec_state(pid) == ZZAPP_REC_FREE) {
		ctx_pid[core] = ZZAPP_PID_NONE;
		return 0;
	}
	ctx_pid[core] = pid;
	return 1;
}

uint32_t zzapp_services_current_pid(void)
{
	return service_pid();
}

#ifndef ZZAPP_HOST_TEST
static uint64_t now_us(void);
static uint64_t reject_report_last_us;
static uint32_t reject_report_held;

/* A rejection prints every time, so a client that refuses all loads leaves a
 * line per refusal. One that retries in a loop is held to four lines a
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
static int service_test_current_thread = -1;
static uint32_t service_loader_generation;
static void (*service_test_loader_begin_hook)(void);
/* What the stub loader's begin returns once its hook ran. */
static int service_test_loader_begin_rc = AAR_OK;
static void (*service_test_loader_set_generation_hook)(void);
static int (*service_test_loader_chunk_hook)(uint32_t file_off,
					      const void *src, uint32_t len);
static int (*service_test_loader_end_hook)(void);
static void (*service_test_deferred_completion_hook)(void);
static void (*service_test_deferred_completing_hook)(void);
static void (*service_test_deferred_reply_hook)(void);
static unsigned service_test_loader_abort_count;


__attribute__((weak)) zzapp_kernel_t *zzapp_runtime_kernel(void)
{
	return &service_test_kernel;
}

__attribute__((weak)) int zzapp_kernel_current_thread(void)
{
	return service_test_current_thread;
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
		return service_test_loader_begin_rc;
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

__attribute__((weak)) uint32_t zzapp_loader_card(void)
{
	return 0U;
}

__attribute__((weak)) int zzapp_loader_abort(void)
{
	service_test_loader_abort_count++;
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

/* The generation of the record's handle: what the loader and the host know
 * as the launch generation, and what a revoked handle no longer matches. */
uint32_t zzapp_services_generation(void)
{
	zzapp_rec_t *rec = service_rec();

	return rec ? aar_handle_generation(rec->hdr.handle) : 0U;
}

uint32_t zzapp_services_state(void)
{
	zzapp_rec_t *rec = service_rec();

	return rec ? rec->claim.state : ZZAPP_ST_IDLE;
}

/* The stop latched on the record (by the host: STOP, RELEASE, lease, cancel)
 * or asked for by the client in its control block word. A word seen by a
 * wait that ends on it is latched on the record too, so aar_should_stop()
 * reads true as soon as the wait returns, without waiting for the tick. */
static int stop_requested_rec(zzapp_rec_t *rec)
{
	if (!rec)
		return 0;
	if (__atomic_load_n(&rec->host.stop_requested, __ATOMIC_ACQUIRE) != 0U)
		return 1;
	if (zzapp_xport_live(rec) &&
	    zzapp_xport_cb_read_client(rec, AAR_CB_STOP_REQUEST) != 0U) {
		zzapp_host_latch_stop(rec->hdr.pid);
		return 1;
	}
	return 0;
}

int zzapp_services_stop_requested(void)
{
	return stop_requested_rec(service_rec());
}

/* Publish the record's state and end reason in the control block. */
static void cb_publish_state(zzapp_rec_t *rec, uint32_t state)
{
	zzapp_xport_cb_set_state(rec, state, rec->claim.terminal_reason);
}

/* OR bits into AAR_CB_FLAGS (the console has been used, a display is open).
 * The host sets the display, audio and GL bits itself when it loads. */
static void cbf_or(zzapp_rec_t *rec, uint32_t bits)
{
	slot_state_lock_acquire();
	zzapp_xport_cb_put(rec, AAR_CB_FLAGS,
			   zzapp_xport_cb_get(rec, AAR_CB_FLAGS) | bits);
	slot_state_lock_release();
}

static void cbf_or_current(uint32_t bits)
{
	zzapp_rec_t *rec = service_rec();

	if (rec)
		cbf_or(rec, bits);
}

/* Caller holds g_slot_state_lock. */
static void set_state_locked(zzapp_rec_t *rec, uint32_t state)
{
	rec->claim.state = state;
	zzapp_loader_set_state(state);
	cb_publish_state(rec, state);
}

/* Caller holds g_slot_state_lock. */
static void apply_state_locked(uint32_t state)
{
	zzapp_rec_t *rec = service_rec();

	if (!rec) {
		zzapp_loader_set_state(state);
		return;
	}
	if (state == ZZAPP_ST_IDLE &&
	    zzapp_xport_flag_test(rec, ZZAPP_XPORT_F_QUARANTINED) &&
	    zzapp_rec_state(rec->hdr.pid) != ZZAPP_REC_RECLAIMED) {
		/* A quarantined claim reads IDLE once the shell is held
		 * (retire_wire). */
		rec->claim.state = ZZAPP_ST_STOPPING;
		cb_publish_state(rec, ZZAPP_ST_STOPPING);
	} else if (state == ZZAPP_ST_IDLE && runtime_of(rec)->defer.cancelled &&
		   runtime_of(rec)->defer.state == DEFER_RUNNING) {
		zzapp_loader_set_state(state);
	} else {
		set_state_locked(rec, state);
	}
}

/* Publishes the state of the selected record. IDLE of a claimed record is
 * published by zzapp_services_retire_wire(), which also frees or holds the
 * record. */
void zzapp_services_set_state(uint32_t state)
{
	slot_state_lock_acquire();
	apply_state_locked(state);
	slot_state_lock_release();
}

uint32_t zzapp_services_memory_limit(void)
{
	zzapp_rec_t *rec = service_rec();

	return rec ? rec->claim.memory_limit : 0U;
}

uint32_t zzapp_services_host_visible_limit(uint32_t pid)
{
	zzapp_rec_t *rec = zzapp_rec_by_pid(pid);

	if (!rec || zzapp_rec_state(pid) == ZZAPP_REC_FREE)
		return 0U;
	return rec->claim.host_visible_limit;
}

void zzapp_services_set_fault(uint32_t code, uint32_t pc, uint32_t sp,
			      uint32_t lr, uint32_t far, uint32_t fsr,
			      uint32_t cpsr)
{
	zzapp_rec_t *rec = service_rec();

	if (rec)
		zzapp_xport_cb_set_fault(rec, code, fsr, cpsr, pc, sp, lr, far);
}

void zzapp_services_set_exit(int32_t code)
{
	zzapp_rec_t *rec = service_rec();

	if (rec)
		zzapp_xport_cb_put(rec, AAR_CB_EXIT_CODE, (uint32_t)code);
}

void zzapp_services_note_progress(uint32_t lo, uint32_t hi)
{
	zzapp_rec_t *rec = service_rec();

	if (!rec)
		return;
	zzapp_xport_cb_put(rec, AAR_CB_PROGRESS_LO, lo);
	zzapp_xport_cb_put(rec, AAR_CB_PROGRESS_HI, hi);
}

/* The end reason survives in the control block until the claim is released.
 * A lease lost for good marks the claim quarantined here, before the retire
 * that follows returns the app's resources: the client may still write
 * through a mapping of the app's host-visible blocks, so they stay until the
 * shell is released (retire_wire revokes the handle). */
void zzapp_services_set_terminal(uint32_t reason)
{
	zzapp_rec_t *rec = service_rec();

	if (!rec)
		return;
	rec->claim.terminal_reason = reason;
	if (reason == ZZAPP_END_LEASE)
		zzapp_xport_flag_set(rec, ZZAPP_XPORT_F_QUARANTINED);
	zzapp_xport_cb_put(rec, AAR_CB_END_REASON, reason);
}

/* A resource refusal during load. The op that fails replies with the reason
 * (ZZAPP_REFUSAL_REASON). */
void zzapp_services_note_refusal(uint32_t reason)
{
	zzapp_rec_t *rec = service_rec();

	if (rec)
		rec->claim.refusal_reason = reason;
}

/* Caller holds g_slot_state_lock. The claim is cancelled: its limits go, the
 * stop is latched and the state reads STOPPING with the reason. */
static void begin_claim_cancellation(zzapp_rec_t *rec, uint32_t reason)
{
	rec->claim.memory_limit = 0U;
	rec->claim.host_visible_limit = 0U;
	runtime_of(rec)->load_progress = 0U;
	rec->claim.state = ZZAPP_ST_STOPPING;
	rec->claim.terminal_reason = reason;
	zzapp_host_latch_stop(rec->hdr.pid);
	cb_publish_state(rec, ZZAPP_ST_STOPPING);
}

/*
 * Caller holds g_slot_state_lock. The claim's client is gone or silent (lease
 * lost, load expired): the shell is kept, and its handle is revoked, so the
 * old client's next request is stale (and releases the shell), while the new
 * handle LIST reports lets an administrator release it.
 */
static void quarantine_locked(zzapp_rec_t *rec)
{
	struct zzapp_service_runtime *state = runtime_of(rec);

	zzapp_xport_flag_set(rec, ZZAPP_XPORT_F_QUARANTINED);
	if (!state->handle_revoked) {
		(void)zzapp_rec_revoke_handle(rec);
		state->handle_revoked = 1U;
	}
}

/*
 * Caller holds g_slot_state_lock. The transport blocks and the record go
 * back: off every set first, the buffers with their pointers cleared before
 * they are freed (zzapp_xport_release), then the record with its generation
 * bumped (zzapp_rec_free), so no handle of this claim names anything again.
 */
static void release_shell_locked(zzapp_rec_t *rec)
{
	uint32_t handle = rec->hdr.handle;
	uint32_t pid = rec->hdr.pid;

	set_del(&set_svc, pid);
	set_del(&set_core0, pid);
	set_del(&set_display, pid);
	/* A quarantined claim kept the app's own host-visible blocks, which a
	 * lost client may still write; they go with the shell. */
	if (zzapp_xport_flag_test(rec, ZZAPP_XPORT_F_QUARANTINED) &&
	    zzapp_host_revoke_visible) {
		struct svc_ctx saved = ctx_save();

		ctx_pid[service_context()] = pid;
		zzapp_host_revoke_visible();
		ctx_restore(saved);
	}
	(void)zzapp_xport_release(rec);
	(void)zzapp_rec_free(handle);
}

/*
 * Caller holds g_slot_state_lock. A reclaimed shell keeps STOPPING until its
 * audio handoff, deferred reply and ordinary-exit console output have finished.
 * RELEASE discards pending console output; it need not wait for the client
 * that is giving the claim back.
 */
static void finish_shell_release(zzapp_rec_t *rec)
{
	struct zzapp_service_runtime *state = runtime_of(rec);

	if (zzapp_rec_state(rec->hdr.pid) != ZZAPP_REC_RECLAIMED ||
	    __atomic_load_n(&state->defer.state, __ATOMIC_ACQUIRE) != DEFER_IDLE ||
	    audio_handoff_pending(state))
		return;
	if (zzapp_xport_flag_test(rec, ZZAPP_XPORT_F_RELEASE_REQUESTED)) {
		release_shell_locked(rec);
		return;
	}
	if (rec->claim.terminal_reason == ZZAPP_END_EXIT && console_pending(rec))
		return;
	if (rec->claim.state != ZZAPP_ST_IDLE) {
		rec->claim.state = ZZAPP_ST_IDLE;
		cb_publish_state(rec, ZZAPP_ST_IDLE);
	}
}

/*
 * Retire stages 6 and 7, called last by the host's retire. The record becomes
 * RECLAIMED and keeps its handle, its control block, its queues and its
 * buffers: the client drains the messages and reads the final status after
 * the app ended, and a client that lost its lease may still write through
 * its mapping. The state reads IDLE with the end reason once ordinary-exit
 * console output and any audio handoff to core 0 have finished.
 * When the claim's release was requested and nothing is owed, the shell is
 * freed here: 1. Otherwise 0:
 * RELEASE, a stale request of a quarantined claim, or reset frees it later.
 * Replays are safe: a record that is already free reports 1.
 */
int zzapp_services_retire_wire(struct zzapp_rec *rec)
{
	uint32_t pid;

	if (!rec)
		return 1;
	pid = rec->hdr.pid;
	slot_state_lock_acquire();
	if (rec->hdr.handle == 0U || zzapp_rec_state(pid) == ZZAPP_REC_FREE) {
		slot_state_lock_release();
		return 1;
	}
	/* An app whose lease ran out ends here with its client unaccounted for. */
	if (rec->claim.terminal_reason == ZZAPP_END_LEASE)
		zzapp_xport_flag_set(rec, ZZAPP_XPORT_F_QUARANTINED);
	if (zzapp_xport_flag_test(rec, ZZAPP_XPORT_F_QUARANTINED))
		quarantine_locked(rec);
	(void)zzapp_rec_set_state(rec, ZZAPP_REC_RECLAIMED);
	if (audio_handoff_pending(runtime_of(rec)) ||
	    (rec->claim.terminal_reason == ZZAPP_END_EXIT && console_pending(rec))) {
		rec->claim.state = ZZAPP_ST_STOPPING;
		cb_publish_state(rec, ZZAPP_ST_STOPPING);
	} else {
		rec->claim.state = ZZAPP_ST_IDLE;
		cb_publish_state(rec, ZZAPP_ST_IDLE);
	}
	finish_shell_release(rec);
	slot_state_lock_release();
	return zzapp_rec_state(pid) == ZZAPP_REC_FREE ? 1 : 0;
}

/* Caller holds g_slot_state_lock. 0: core 1 keeps a RUNNING op and completes
 * its cancellation; -1: a completed op the client has not taken, preserved;
 * 1: nothing runs, a queued op was dropped. */
static int stop_current_deferred(zzapp_rec_t *rec)
{
	struct zzapp_deferred *defer = &runtime_of(rec)->defer;

	if (defer->state == DEFER_RUNNING) {
		defer->cancelled = 1U;
		return 0;
	}
	if (defer->state == DEFER_DONE)
		return -1;
	if (defer->state != DEFER_IDLE) {
		defer->state = DEFER_IDLE;
		if (deferred_harvest_pid == rec->hdr.pid)
			deferred_harvest_pid = ZZAPP_PID_NONE;
	}
	return 1;
}

/* Caller holds g_slot_state_lock. Nonzero: core 1 keeps a RUNNING op and
 * completes its cancellation; a queued or finished one is dropped. */
static int discard_deferred(zzapp_rec_t *rec)
{
	struct zzapp_deferred *defer = &runtime_of(rec)->defer;

	if (defer->state == DEFER_RUNNING) {
		defer->cancelled = 1U;
		return 1;
	}
	if (defer->state != DEFER_IDLE) {
		defer->state = DEFER_IDLE;
		if (deferred_harvest_pid == rec->hdr.pid)
			deferred_harvest_pid = ZZAPP_PID_NONE;
	}
	return 0;
}

/* The client's lease counter moved: the claim is alive, its lease restarts. */
static void renew_claim_lease(zzapp_rec_t *rec, uint64_t now)
{
	struct zzapp_service_runtime *state = runtime_of(rec);
	uint32_t counter = zzapp_xport_cb_read_client(rec, AAR_CB_LEASE_COUNTER);

	if (counter == state->lease_seen)
		return;
	state->lease_seen = counter;
	state->claim_started = now;
	state->load_progress = now;
}

/*
 * Lease of a claim that is not running yet: 30 s while CLAIMED or LOADING,
 * extended by every change of the client's lease counter, by LOAD_BEGIN and by
 * each completed LOAD_CHUNK. A claim that ran out is cancelled and its shell
 * kept under a revoked handle. One with nothing loaded ends through the one
 * retire path; one whose load is in flight is cancelled and core 1 ends it.
 */
void zzapp_services_expire_claims(uint64_t now)
{
	struct svc_ctx saved = ctx_save();
	uint32_t i;

	for (i = set_core0.count; i-- > 0U;) {
		zzapp_rec_t *rec;
		uint32_t retire_pid = ZZAPP_PID_NONE;
		uint32_t pid;
		uint64_t deadline;
		int loading;

		slot_state_lock_acquire();
		if (i >= set_core0.count) {
			slot_state_lock_release();
			continue;
		}
		pid = set_core0.pids[i];
		rec = zzapp_rec_by_pid(pid);
		if (!rec || zzapp_rec_state(pid) != ZZAPP_REC_LIVE) {
			slot_state_lock_release();
			continue;
		}
		ctx_pid[service_context()] = pid;
		if (rec->claim.state == ZZAPP_ST_CLAIMED) {
			renew_claim_lease(rec, now);
			deadline = runtime_of(rec)->claim_started +
				   ZZAPP_CLAIM_LEASE_US;
			loading = runtime_of(rec)->defer.opcode ==
					  ZZAPP_OP_LOAD_BEGIN &&
				  runtime_of(rec)->defer.state == DEFER_RUNNING;
		} else if (rec->claim.state == ZZAPP_ST_LOADING) {
			renew_claim_lease(rec, now);
			deadline = runtime_of(rec)->load_progress +
				   ZZAPP_LOAD_LEASE_US;
			loading = 1;
		} else {
			slot_state_lock_release();
			continue;
		}
		if (now < deadline) {
			slot_state_lock_release();
			continue;
		}
		if (discard_deferred(rec))
			loading = 1;
		quarantine_locked(rec);
		begin_claim_cancellation(rec, ZZAPP_END_LEASE);
		if (loading) {
			rec->claim.terminal_reason = ZZAPP_END_QUARANTINED;
			zzapp_xport_cb_put(rec, AAR_CB_END_REASON,
					   ZZAPP_END_QUARANTINED);
		} else {
			retire_pid = pid;
		}
		slot_state_lock_release();
		/* Nothing was loaded and core 1 has no stake: the claim ends
		 * through the one retire path. */
		if (retire_pid != ZZAPP_PID_NONE)
			(void)zzapp_rec_retire(retire_pid);
	}
	ctx_restore(saved);
}

static int grant_user_buffer(uint32_t address, uint32_t bytes,
			     uint32_t *grant_base, uint32_t *grant_bytes,
			     uint32_t *user_address)
{
	zzapp_proc_t *proc = service_proc();
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
	zzapp_proc_t *proc = service_proc();
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
/* Optional models of the host-visible heap QUERY reports. */
extern uint32_t zzapp_test_hostheap_free(void) __attribute__((weak));
extern uint32_t zzapp_test_hostheap_largest(void) __attribute__((weak));
#endif

static void audio_detach(void);
static void audio_post_teardown(void);
static void display_unbind(zzapp_rec_t *rec);
static void console_service(zzapp_rec_t *rec);

void zzapp_services_on_launch(void)
{
	struct zzapp_service_runtime *state = current_runtime();

	if (!state)
		return;
	state->log_ring_count = 0U;
	state->log_ring_window = 0U;
#ifndef ZZAPP_HOST_TEST
	state->log_uart_count = 0U;
	state->log_uart_window = 0U;
#endif
	slot_state_lock_acquire();
	state->display_present_us = 0U;
	if (state->present_feedback.sequence != 0U)
		memset(&state->present_feedback, 0, sizeof(state->present_feedback));
	state->present_submitting = 0;
	slot_state_lock_release();
}

/* Retire stage 3 for the services' share of an app: display grants and
 * surface lease, the audio handoff, files, waits, requests in flight, the
 * load buffer. The queues and the other buffers stay: the client drains them
 * until it releases the claim. The state moves to STOPPING; IDLE is
 * published only by retire_wire. */
void zzapp_services_on_app_end(void)
{
	struct zzapp_service_runtime *state = current_runtime();
	zzapp_rec_t *rec = service_rec();

	if (!state || !rec)
		return;
	if (!zzapp_display_mailbox_retire_ready())
		return;
	display_mailbox_disable();
	audio_post_teardown();
	display_unbind(rec);
	revoke_surface_grants();
	zzapp_kernel_surface_unbind(zzapp_runtime_kernel(), service_pid());
	if (state->display_back_owned && state->display_back != 0U)
		zzapp_loader_free_back(state->display_back);
	state->display_back_owned = 0;
	/* What the app was waiting for is no longer wanted: a completion that
	 * still arrives is dropped by id. */
	zzapp_xport_abandon(rec, ZZAPP_XPORT_REQ_BLOCKING);
	zzapp_xport_abandon(rec, ZZAPP_XPORT_REQ_FLIP);
	zzapp_xport_abandon(rec, ZZAPP_XPORT_REQ_PALETTE);
	zzapp_xport_abandon(rec, ZZAPP_XPORT_REQ_FILE_ASYNC);
	memset(&state->file_io, 0, sizeof(state->file_io));
	wait_end();
	memset(state->file_open, 0, sizeof(state->file_open));
	memset(state->file_access, 0, sizeof(state->file_access));
	memset(state->list_open, 0, sizeof(state->list_open));
	(void)zzapp_xport_block_free(rec, ZZAPP_XPORT_BLOCK_LOAD);
	/* Console output the app wrote before it ended still goes out. */
	console_service(rec);
	slot_state_lock_acquire();
	if (state->present_feedback.sequence != 0U)
		memset(&state->present_feedback, 0, sizeof(state->present_feedback));
	state->present_submitting = 0;
	rec->claim.state = ZZAPP_ST_STOPPING;
	cb_publish_state(rec, ZZAPP_ST_STOPPING);
	slot_state_lock_release();
}

/*
 * Amiga reset, once, after the host retired every record and before the
 * allocator is reset: every shell is released and gives its buffers back.
 * The quarantine flag of a quarantined shell stays until its release, so the
 * release returns the app's own host-visible blocks as well. A shell whose
 * audio detach is still owed to core
 * 0 stays until core 0 finishes it, and one whose deferred op core 1 still
 * completes until that is done; both go as soon as they can, because the
 * release is requested. Handles stay stale: freeing a record bumps its
 * generation.
 */
void zzapp_services_on_reset(void)
{
	struct svc_ctx saved = ctx_save();
	uint32_t pid;

#ifndef ZZAPP_HOST_TEST
	printf("[app] reset: claims released\r\n");
#endif
	for (pid = 0U; pid < zzapp_rec_capacity(); pid++) {
		zzapp_rec_t *rec;

		if (zzapp_rec_state(pid) == ZZAPP_REC_FREE)
			continue;
		rec = zzapp_rec_by_pid(pid);
		ctx_pid[service_context()] = pid;
		if (zzapp_rec_state(pid) != ZZAPP_REC_RECLAIMED) {
			zzapp_services_on_app_end();
			(void)zzapp_rec_retire(pid);
		}
		if (zzapp_rec_state(pid) == ZZAPP_REC_FREE)
			continue;
		slot_state_lock_acquire();
		/* The quarantine stays until the shell is released: release then
		 * returns the app's own host-visible blocks with it. */
		zzapp_xport_flag_set(rec, ZZAPP_XPORT_F_RELEASE_REQUESTED);
		if (discard_deferred(rec))
			/* Core 1 still owns the cancelled op; completing it is
			 * the point where the shell can go. */
			rec->claim.state = ZZAPP_ST_STOPPING;
		runtime_of(rec)->defer.start_pending = 0U;
		finish_shell_release(rec);
		slot_state_lock_release();
	}
	ctx_restore(saved);
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

/* 1 is a live fd. 2 is retired by a timeout: aar-run may still complete the
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
	case AAR_E_NORESOURCE:
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

/* ---- Requests to the client ------------------------------------------------ */

/* A request that is already complete costs no scheduler round trip: the pump
 * is polled this many times before the caller parks (the pattern of
 * FLIP_POLL_ACKS, which the swquake flip wait depends on). */
#define ZZAPP_REQ_POLL_PUMPS 8U

/*
 * Post a request of the table entry `kind`. The SQ can be full when the
 * client has stopped draining: the caller then parks (ZZAPP_WAIT_SQ_SPACE)
 * until the system pass sees room. Requests never fail on a full SQ. AAR_OK,
 * AAR_E_BUSY when the table entry itself is still in use, AAR_E_IO after a
 * stop or on corrupt indices.
 */
static int req_post_prepared(zzapp_xport_req_kind_t kind,
			     const aar_request *req, uint64_t deadline,
			     uint32_t *id, int (*prepare)(void *), void *context)
{
	zzapp_rec_t *rec = service_rec();

	for (;;) {
		int rc = prepare ?
			zzapp_xport_post_request_prepared(rec, kind, req, deadline,
							   id, prepare, context) :
			zzapp_xport_post_request(rec, kind, req, deadline, id);
		int space;

		if (rc != AAR_E_BUSY) {
			wait_end();
			return rc;
		}
		space = zzapp_xport_sq_space(rec);
		if (space < 0) {
			wait_end();
			return AAR_E_IO;
		}
		if (space != 0) {
			wait_end();
			return AAR_E_BUSY;
		}
		if (zzapp_services_stop_requested()) {
			wait_end();
			return AAR_E_IO;
		}
		wait_begin(ZZAPP_WAIT_SQ_SPACE, 0U, 0U);
		yield_wait_once();
		rc = wait_status();
		if (rc != AAR_OK) {
			wait_end();
			return rc;
		}
	}
}

static int req_post(zzapp_xport_req_kind_t kind, const aar_request *req,
		    uint64_t deadline, uint32_t *id)
{
	return req_post_prepared(kind, req, deadline, id, NULL, NULL);
}

/*
 * Wait for the completion of request `id` of entry `kind`: pump, test, a
 * bounded poll before parking, then park under `wk` (the system pass wakes
 * the app when the completion is in). `deadline` is absolute microseconds, 0
 * for none (console, display and audio waits end only by completion or stop).
 * A request that is given up on (timeout, stop) is abandoned: its id is
 * remembered and the late completion is dropped by the pump.
 */
static int req_wait(zzapp_xport_req_kind_t kind, enum zzapp_wait_kind wk,
		    uint32_t id, uint64_t deadline, aar_completion *c)
{
	zzapp_rec_t *rec = service_rec();
	uint32_t poll;
	int rc;

	for (poll = 0U; poll < ZZAPP_REQ_POLL_PUMPS; poll++) {
		if (zzapp_xport_pump(rec) < 0)
			goto io_error;
		rc = zzapp_xport_take_completion(rec, kind, c);
		if (rc == 1)
			return AAR_OK;
		if (rc < 0)
			goto io_error;
	}
	for (;;) {
		int status;

		if (zzapp_xport_pump(rec) < 0)
			goto io_error;
		rc = zzapp_xport_take_completion(rec, kind, c);
		if (rc == 1) {
			wait_end();
			return AAR_OK;
		}
		if (rc < 0)
			goto io_error;
		if (zzapp_services_stop_requested()) {
			zzapp_xport_abandon(rec, kind);
			wait_end();
			return AAR_E_IO;
		}
		if (deadline != 0U && now_us() >= deadline) {
			zzapp_xport_abandon(rec, kind);
			wait_end();
			return AAR_E_TIMEDOUT;
		}
		wait_begin(wk, id, deadline);
		yield_wait_once();
		status = wait_status();
		if (status == AAR_E_TIMEDOUT) {
			/* A completion that landed with the timeout still counts. */
			if (zzapp_xport_pump(rec) >= 0 &&
			    zzapp_xport_take_completion(rec, kind, c) == 1) {
				wait_end();
				return AAR_OK;
			}
			zzapp_xport_abandon(rec, kind);
			wait_end();
			return AAR_E_TIMEDOUT;
		}
		if (status != AAR_OK) {
			zzapp_xport_abandon(rec, kind);
			wait_end();
			return status;
		}
	}
io_error:
	zzapp_xport_abandon(rec, kind);
	wait_end();
	return AAR_E_IO;
}

/*
 * One blocking request of the app: post, wait, and the completion's result.
 * `args` are the request's arg words, `buf` a payload buffer of the claim (a
 * service area or a block) and `buf_len` the bytes of it that matter. Returns
 * the wait's failure, or the completion's result (AAR_OK or an AAR_E_*);
 * `out`, when given, holds the completion either way.
 */
static int blocking_request_flags(uint32_t op, uint16_t flags,
				  enum zzapp_wait_kind wk, uint64_t deadline,
				  const uint32_t *args, uint32_t nargs,
				  const zzapp_xport_buf_t *buf,
				  uint32_t buf_len, aar_completion *out)
{
	aar_completion local;
	aar_request req;
	uint32_t id;
	uint32_t i;
	int rc;

	if (!out)
		out = &local;
	memset(&req, 0, sizeof(req));
	req.op = (uint16_t)op;
	req.flags = flags;
	for (i = 0U; i < nargs && i < AAR_ENTRY_ARGS; i++)
		req.arg[i] = args[i];
	if (buf) {
		req.buf = buf->token;
		req.buf_off = buf->off;
		req.buf_len = buf_len;
	}
	rc = req_post(ZZAPP_XPORT_REQ_BLOCKING, &req, deadline, &id);
	if (rc != AAR_OK)
		return rc;
	rc = req_wait(ZZAPP_XPORT_REQ_BLOCKING, wk, id, deadline, out);
	if (rc != AAR_OK)
		return rc;
	return out->result < 0 ? out->result : AAR_OK;
}

static int blocking_request(uint32_t op, enum zzapp_wait_kind wk,
			    uint64_t deadline, const uint32_t *args,
			    uint32_t nargs, const zzapp_xport_buf_t *buf,
			    uint32_t buf_len, aar_completion *out)
{
	return blocking_request_flags(op, 0U, wk, deadline, args, nargs, buf,
				      buf_len, out);
}

/* ---- Files ---------------------------------------------------------------- */

static int file_request(uint32_t op, const uint32_t *args, uint32_t nargs,
			const zzapp_xport_buf_t *buf, uint32_t buf_len,
			aar_completion *c)
{
	return blocking_request(op, ZZAPP_WAIT_FILE,
				now_us() + ZZAPP_FILE_TIMEOUT_US, args, nargs,
				buf, buf_len, c);
}

/* An ended ticket no longer owns app memory, but the DOS packet may still
 * own the native fd and transport block. */
static int file_async_abandoned(zzapp_rec_t *rec)
{
	const uint8_t *names = &rec->xport.req[ZZAPP_XPORT_REQ_FILE_ASYNC].abandoned_names;

	if (__atomic_load_n(names, __ATOMIC_ACQUIRE) == 0U)
		return 0;
	(void)zzapp_xport_pump(rec);
	return __atomic_load_n(names, __ATOMIC_ACQUIRE) != 0U;
}

static int file_async_fd_busy(zzapp_rec_t *rec, int fd)
{
	const struct zzapp_file_io *io = &runtime_of(rec)->file_io;

	return io->fd == fd && (io->ticket != 0U || file_async_abandoned(rec));
}

/* Unlike an ordinary SVC argument, this address can outlive a return to
 * userspace. Recheck it before copying to or from it on a later SVC. */
static int file_async_check_buffer(zzapp_rec_t *rec, const void *buf, uint32_t len,
				   int write)
{
#ifndef ZZAPP_HOST_TEST
	return zzapp_svc_check_user(&rec->proc.as, (uint32_t)(uintptr_t)buf, len, write);
#else
	(void)rec;
	(void)buf;
	(void)len;
	(void)write;
	return AAR_OK; /* fixture buffers are ordinary host memory */
#endif
}

static int file_async_post(zzapp_rec_t *rec, struct zzapp_file_io *io)
{
	aar_request req = {0};
	zzapp_xport_buf_t data;
	uint32_t chunk = io->len - io->count;
	int rc = zzapp_xport_sq_space(rec);

	if (rc <= 0)
		return rc < 0 ? rc : AAR_E_BUSY;
	if (chunk > ZZAPP_XPORT_DATA_MAX)
		chunk = ZZAPP_XPORT_DATA_MAX;
	if (io->op == AAR_OP_FILE_WRITE && io->count != 0U &&
	    file_async_check_buffer(rec, (const uint8_t *)io->buf + io->count,
				    chunk, 0) != AAR_OK)
		return AAR_E_FAULT;
	rc = zzapp_xport_data_reserve(rec, chunk, &data);
	if (rc != AAR_OK)
		return rc;
	if (io->op == AAR_OP_FILE_WRITE)
		memcpy(data.ptr, (const uint8_t *)io->buf + io->count, chunk);
	req.op = io->op;
	req.flags = AAR_FILEF_ASYNC;
	req.arg[AAR_A_FILE_FD] = (uint32_t)io->fd;
	req.arg[AAR_A_FILE_LEN] = chunk;
	req.buf = data.token;
	req.buf_off = data.off;
	req.buf_len = chunk;
	rc = zzapp_xport_post_request(rec, ZZAPP_XPORT_REQ_FILE_ASYNC, &req,
				      now_us() + ZZAPP_FILE_TIMEOUT_US, NULL);
	if (rc == AAR_OK) {
		io->chunk = chunk;
		slot_state_lock_acquire();
		set_add(&set_svc, service_pid());
		slot_state_lock_release();
	}
	return rc;
}

static int file_async_begin(int fd, void *buf, uint32_t len, uint32_t *ticket,
			    uint16_t op, uint8_t access)
{
	zzapp_rec_t *rec = service_rec();
	struct zzapp_service_runtime *state;
	struct zzapp_file_io next = {0};
	int rc;

	if (!ticket || (!buf && len != 0U))
		return AAR_E_INVAL;
	if (!rec || zzapp_services_stop_requested())
		return AAR_E_IO;
	state = runtime_of(rec);
	if (!slot_live(state->file_open, fd) ||
	    (state->file_access[fd_index(fd)] & access) == 0U)
		return AAR_E_BADF;
	if (state->file_io.ticket != 0U || file_async_abandoned(rec))
		return AAR_E_BUSY;
	if (state->file_io.serial == UINT32_MAX)
		return AAR_E_NOSPC;
	next.serial = state->file_io.serial + 1U;
	next.ticket = next.serial;
	next.fd = fd;
	next.op = op;
	next.buf = buf;
	next.len = len;
	next.finished = len == 0U;
	if (len != 0U) {
		rc = file_async_post(rec, &next);
		if (rc != AAR_OK)
			return rc;
	}
	state->file_io = next;
	*ticket = next.ticket;
	return AAR_OK;
}

int zzapp_svc_file_read_async(int fd, void *buf, uint32_t len, uint32_t *ticket)
{
	return file_async_begin(fd, buf, len, ticket, AAR_OP_FILE_READ, AAR_FILE_READ);
}

int zzapp_svc_file_write_async(int fd, const void *buf, uint32_t len, uint32_t *ticket)
{
	return file_async_begin(fd, (void *)buf, len, ticket, AAR_OP_FILE_WRITE,
				AAR_FILE_WRITE);
}

static int file_async_pending(const zzapp_rec_t *rec)
{
	return zzapp_xport_req_state(rec, ZZAPP_XPORT_REQ_FILE_ASYNC) ==
	       ZZAPP_XPORT_REQ_POSTED;
}

/* The pump runs first; a queued completion wins over the deadline. */
static void file_async_check(zzapp_rec_t *rec, int pump_rc)
{
	struct zzapp_file_io *io = &runtime_of(rec)->file_io;
	int result;

	if (!file_async_pending(rec))
		return;
	if (pump_rc < 0 || stop_requested_rec(rec))
		result = AAR_E_IO;
	else if (now_us() >= zzapp_xport_req_deadline(rec, ZZAPP_XPORT_REQ_FILE_ASYNC))
		result = AAR_E_TIMEDOUT;
	else
		return;
	zzapp_xport_abandon(rec, ZZAPP_XPORT_REQ_FILE_ASYNC);
	io->result = result;
	io->finished = 1;
}

int zzapp_svc_file_collect(uint32_t ticket, int32_t *status, uint32_t *out_n)
{
	zzapp_rec_t *rec = service_rec();
	struct zzapp_file_io *io;

	if (!ticket || !status || !out_n)
		return AAR_E_INVAL;
	if (!rec)
		return AAR_E_IO;
	io = &runtime_of(rec)->file_io;
	if (ticket != io->ticket)
		return AAR_E_STALE;

	if (!io->finished) {
		aar_completion c;
		int pump_rc = zzapp_xport_pump(rec);

		file_async_check(rec, pump_rc);
		if (!io->finished && zzapp_xport_take_completion(
			    rec, ZZAPP_XPORT_REQ_FILE_ASYNC, &c) == 1) {
			uint32_t n = c.out[AAR_R_FILE_COUNT];
			zzapp_xport_buf_t data = {0};

			if (n > io->chunk || c.result > AAR_OK ||
			    (n && io->op == AAR_OP_FILE_READ &&
			     !zzapp_xport_block_get(rec, ZZAPP_XPORT_BLOCK_DATA, &data))) {
				io->result = AAR_E_IO;
				io->finished = 1;
			} else if (n && io->op == AAR_OP_FILE_READ &&
				   file_async_check_buffer(rec, (uint8_t *)io->buf + io->count,
							   n, 1) != AAR_OK) {
				io->result = AAR_E_FAULT;
				io->finished = 1;
			} else {
				if (n && io->op == AAR_OP_FILE_READ)
					memcpy((uint8_t *)io->buf + io->count, data.ptr, n);
				io->count += n;
				io->result = c.result;
				io->finished = c.result < 0 || n < io->chunk || io->count == io->len;
			}
		}
		if (!io->finished && !file_async_pending(rec)) {
			int rc = pump_rc < 0 || zzapp_services_stop_requested()
				 ? AAR_E_IO : file_async_post(rec, io);

			if (rc < 0 && rc != AAR_E_BUSY) {
				io->result = rc;
				io->finished = 1;
			}
		}
	}
	if (!io->finished)
		return AAR_E_BUSY;
	*status = io->result;
	*out_n = io->count;
	io->buf = NULL;
	io->ticket = 0U;
	return AAR_OK;
}

/* The path goes to the scratch area as bytes with a NUL. */
static int put_path(const char *path, zzapp_xport_buf_t *buf, uint32_t *len)
{
	size_t n;
	int rc;

	if (!path)
		return AAR_E_INVAL;
	n = strlen(path);
	if (n == 0U || n > ZZAPP_SVC_PATH_MAX)
		return AAR_E_INVAL;
	rc = zzapp_xport_service_area(service_rec(), ZZAPP_XPORT_AREA_SCRATCH, buf);
	if (rc != AAR_OK)
		return rc;
	memcpy(buf->ptr, path, n + 1U);
	*len = (uint32_t)(n + 1U);
	return AAR_OK;
}

int zzapp_svc_file_request(uint32_t flags, const char *title,
			   const char *initial_path, char *path, uint32_t capacity)
{
	zzapp_rec_t *rec = service_rec();
	zzapp_xport_buf_t buf;
	aar_completion c;
	uint32_t args[3];
	uint32_t in_bytes, len;
	size_t title_len, initial_len;
	int rc;

	_Static_assert(2U * (AAR_PATH_MAX + 1U) <= ZZAPP_XPORT_SCRATCH_BYTES,
		       "file requester input fits scratch");
	if ((flags & ~AAR_FILE_REQUEST_SAVE) != 0U || !path || capacity == 0U)
		return AAR_E_INVAL;
	if (!rec || stop_requested_rec(rec))
		return AAR_E_IO;
	title = title ? title : "";
	initial_path = initial_path ? initial_path : "";
	title_len = strlen(title);
	initial_len = strlen(initial_path);
	if (title_len > AAR_PATH_MAX || initial_len > AAR_PATH_MAX)
		return AAR_E_INVAL;
	in_bytes = (uint32_t)(title_len + initial_len + 2U);
	if (capacity > AAR_PATH_MAX + 1U)
		capacity = AAR_PATH_MAX + 1U;
	rc = zzapp_xport_service_area(rec, ZZAPP_XPORT_AREA_SCRATCH, &buf);
	if (rc != AAR_OK)
		return rc;
	memcpy(buf.ptr, title, title_len + 1U);
	memcpy((char *)buf.ptr + title_len + 1U, initial_path, initial_len + 1U);
	args[AAR_A_FILE_REQUEST_FLAGS] = flags;
	args[AAR_A_FILE_REQUEST_IN_BYTES] = in_bytes;
	args[AAR_A_FILE_REQUEST_CAP] = capacity;
	rc = blocking_request(AAR_OP_FILE_REQUEST, ZZAPP_WAIT_FILE, 0U, args, 3U,
			      &buf, in_bytes > capacity ? in_bytes : capacity, &c);
	if (rc != AAR_OK)
		return rc;
	if (c.result == 0)
		return 0;
	len = c.out[AAR_R_FILE_REQUEST_LEN];
	if (c.result != 1 || len == 0U || len >= capacity ||
	    memchr(buf.ptr, '\0', len + 1U) != (char *)buf.ptr + len)
		return AAR_E_IO;
	memcpy(path, buf.ptr, len + 1U);
	return 1;
}

/* AAR file open flags (spec 7.1). At least one of READ and WRITE; CREATE
 * and TRUNCATE need WRITE; EXCL needs CREATE; no other bit. */
static int file_flags_valid(uint32_t flags)
{
	if ((flags & ~(AAR_FILE_READ | AAR_FILE_WRITE | AAR_FILE_CREATE |
		       AAR_FILE_TRUNCATE | AAR_FILE_EXCL)) != 0U)
		return 0;
	if ((flags & (AAR_FILE_READ | AAR_FILE_WRITE)) == 0U)
		return 0;
	if ((flags & (AAR_FILE_CREATE | AAR_FILE_TRUNCATE)) != 0U &&
	    (flags & AAR_FILE_WRITE) == 0U)
		return 0;
	if ((flags & AAR_FILE_EXCL) != 0U && (flags & AAR_FILE_CREATE) == 0U)
		return 0;
	return 1;
}

int zzapp_svc_file_open(const char *path, uint32_t flags)
{
	struct zzapp_service_runtime *state = current_runtime();
	zzapp_xport_buf_t buf;
	uint32_t args[2];
	uint32_t len = 0U;
	int fd;
	int rc;

	if (!service_rec())
		return AAR_E_IO;
	if (!file_flags_valid(flags))
		return AAR_E_INVAL;
	rc = put_path(path, &buf, &len);
	if (rc != AAR_OK)
		return rc;
	fd = alloc_fd(state->file_open);
	if (fd < 0)
		return AAR_E_NOMEM;
	state->file_access[fd_index(fd)] =
		(uint8_t)(flags & (AAR_FILE_READ | AAR_FILE_WRITE));
	args[AAR_A_FILE_OPEN_FD] = (uint32_t)fd;
	args[AAR_A_FILE_OPEN_FLAGS] = flags;
	rc = file_request(AAR_OP_FILE_OPEN, args, 2U, &buf, len, NULL);
	if (rc != AAR_OK) {
		release_slot(state->file_open, fd, rc == AAR_E_TIMEDOUT);
		return rc;
	}
	return fd;
}

int zzapp_svc_file_close(int fd)
{
	struct zzapp_service_runtime *state = current_runtime();
	uint32_t arg = (uint32_t)fd;
	int rc;

	if (!slot_live(state->file_open, fd))
		return AAR_E_BADF;
	if (file_async_fd_busy(service_rec(), fd))
		return AAR_E_BUSY;
	rc = file_request(AAR_OP_FILE_CLOSE, &arg, 1U, NULL, 0U, NULL);
	release_slot(state->file_open, fd, rc == AAR_E_TIMEDOUT);
	return rc;
}

/* Reads and writes go through the data block, one request per chunk of up to
 * ZZAPP_XPORT_DATA_MAX bytes. The bytes the client wrote were stored through
 * the card's coherent port: no cache maintenance on this side. */
int zzapp_svc_file_read(int fd, void *buf, uint32_t len, uint32_t *out_n)
{
	struct zzapp_service_runtime *state = current_runtime();
	zzapp_rec_t *rec = service_rec();
	uint8_t *dst = (uint8_t *)buf;
	uint32_t got = 0U;

	if (!out_n)
		return AAR_E_INVAL;
	*out_n = 0U;
	if (!slot_live(state->file_open, fd) || !dst)
		return AAR_E_BADF;
	/* The id was opened without READ (spec 7.1). */
	if ((state->file_access[fd_index(fd)] & AAR_FILE_READ) == 0U)
		return AAR_E_BADF;
	if (file_async_fd_busy(rec, fd))
		return AAR_E_BUSY;
	while (got < len) {
		zzapp_xport_buf_t data;
		aar_completion c;
		uint32_t args[2];
		uint32_t chunk = len - got;
		uint32_t n;
		int rc;

		if (chunk > ZZAPP_XPORT_DATA_MAX)
			chunk = ZZAPP_XPORT_DATA_MAX;
		rc = zzapp_xport_data_reserve(rec, chunk, &data);
		if (rc != AAR_OK)
			return rc;
		args[AAR_A_FILE_FD] = (uint32_t)fd;
		args[AAR_A_FILE_LEN] = chunk;
		rc = file_request(AAR_OP_FILE_READ, args, 2U, &data, chunk, &c);
		if (rc != AAR_OK)
			return rc;
		n = c.out[AAR_R_FILE_COUNT];
		if (n > chunk)
			n = chunk;
		if (n != 0U)
			memcpy(dst + got, data.ptr, n);
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
	zzapp_rec_t *rec = service_rec();
	const uint8_t *src = (const uint8_t *)buf;
	uint32_t got = 0U;

	if (!out_n)
		return AAR_E_INVAL;
	*out_n = 0U;
	if (!slot_live(state->file_open, fd) || (!src && len != 0U))
		return AAR_E_BADF;
	/* The id was opened without WRITE (spec 7.1). */
	if ((state->file_access[fd_index(fd)] & AAR_FILE_WRITE) == 0U)
		return AAR_E_BADF;
	if (file_async_fd_busy(rec, fd))
		return AAR_E_BUSY;
	while (got < len) {
		zzapp_xport_buf_t data;
		aar_completion c;
		uint32_t args[2];
		uint32_t chunk = len - got;
		uint32_t n;
		int rc;

		if (chunk > ZZAPP_XPORT_DATA_MAX)
			chunk = ZZAPP_XPORT_DATA_MAX;
		rc = zzapp_xport_data_reserve(rec, chunk, &data);
		if (rc != AAR_OK)
			return rc;
		memcpy(data.ptr, src + got, chunk);
		args[AAR_A_FILE_FD] = (uint32_t)fd;
		args[AAR_A_FILE_LEN] = chunk;
		rc = file_request(AAR_OP_FILE_WRITE, args, 2U, &data, chunk, &c);
		if (rc != AAR_OK)
			return rc;
		n = c.out[AAR_R_FILE_COUNT];
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
	aar_completion c;
	uint32_t args[4];
	int rc;

	if (!slot_live(state->file_open, fd))
		return AAR_E_BADF;
	if (file_async_fd_busy(service_rec(), fd))
		return AAR_E_BUSY;
	if (whence != AAR_SEEK_SET && whence != AAR_SEEK_CUR &&
	    whence != AAR_SEEK_END)
		return AAR_E_INVAL;
	args[AAR_A_FILE_FD] = (uint32_t)fd;
	args[AAR_A_FILE_OFF_HI] = (uint32_t)((uint64_t)off >> 32);
	args[AAR_A_FILE_OFF_LO] = (uint32_t)off;
	args[AAR_A_FILE_WHENCE] = (uint32_t)whence;
	rc = file_request(AAR_OP_FILE_SEEK, args, 4U, NULL, 0U, &c);
	if (rc != AAR_OK)
		return rc;
	if (out_pos)
		*out_pos = (int64_t)(((uint64_t)c.out[AAR_R_FILE_POS_HI] << 32) |
				     c.out[AAR_R_FILE_POS_LO]);
	return AAR_OK;
}

int zzapp_svc_file_stat(const char *path, aar_stat *out)
{
	zzapp_xport_buf_t buf;
	aar_completion c;
	uint32_t len = 0U;
	int rc;

	if (!out)
		return AAR_E_INVAL;
	if (!service_rec())
		return AAR_E_IO;
	rc = put_path(path, &buf, &len);
	if (rc != AAR_OK)
		return rc;
	rc = file_request(AAR_OP_FILE_STAT, NULL, 0U, &buf, len, &c);
	if (rc != AAR_OK)
		return rc;
	out->size = ((uint64_t)c.out[AAR_R_STAT_SIZE_HI] << 32) |
		    c.out[AAR_R_STAT_SIZE_LO];
	out->mtime = (int64_t)(((uint64_t)c.out[AAR_R_STAT_MTIME_HI] << 32) |
			       c.out[AAR_R_STAT_MTIME_LO]);
	out->mode = c.out[AAR_R_STAT_MODE];
	out->protect = c.out[AAR_R_STAT_PROTECT] & 0xffU;
	return AAR_OK;
}

int zzapp_svc_file_list_open(const char *path)
{
	struct zzapp_service_runtime *state = current_runtime();
	zzapp_xport_buf_t buf;
	uint32_t len = 0U;
	uint32_t arg;
	int list;
	int rc;

	if (!service_rec())
		return AAR_E_IO;
	rc = put_path(path, &buf, &len);
	if (rc != AAR_OK)
		return rc;
	list = alloc_fd(state->list_open);
	if (list < 0)
		return AAR_E_NOMEM;
	arg = (uint32_t)list;
	rc = file_request(AAR_OP_LIST_OPEN, &arg, 1U, &buf, len, NULL);
	if (rc != AAR_OK) {
		release_slot(state->list_open, list, rc == AAR_E_TIMEDOUT);
		return rc;
	}
	return list;
}

/* The name comes back NUL-terminated in the scratch area. */
int zzapp_svc_file_list_next(int list, aar_dirent *out)
{
	struct zzapp_service_runtime *state = current_runtime();
	zzapp_xport_buf_t buf;
	aar_completion c;
	uint32_t arg = (uint32_t)list;
	uint32_t n;
	int rc;

	if (!slot_live(state->list_open, list) || !out)
		return AAR_E_BADF;
	rc = zzapp_xport_service_area(service_rec(), ZZAPP_XPORT_AREA_SCRATCH, &buf);
	if (rc != AAR_OK)
		return rc;
	rc = file_request(AAR_OP_LIST_NEXT, &arg, 1U, &buf, ZZAPP_SVC_NAME_ROOM,
			  &c);
	if (rc != AAR_OK)
		return rc;
	for (n = 0U; n < ZZAPP_SVC_NAME_ROOM - 1U &&
		     ((const char *)buf.ptr)[n] != '\0'; n++)
		;
	memcpy(out->name, buf.ptr, n);
	out->name[n] = '\0';
	out->mode = c.out[AAR_R_LIST_MODE];
	return AAR_OK;
}

int zzapp_svc_file_list_close(int list)
{
	struct zzapp_service_runtime *state = current_runtime();
	uint32_t arg = (uint32_t)list;
	int rc;

	if (!slot_live(state->list_open, list))
		return AAR_E_BADF;
	rc = file_request(AAR_OP_LIST_CLOSE, &arg, 1U, NULL, 0U, NULL);
	release_slot(state->list_open, list, rc == AAR_E_TIMEDOUT);
	return rc;
}

/* ---- Files, minor 6 --------------------------------------------------------- */

/* Path operations take the path in the scratch area; the request borrows it
 * until its completion. A wait that ends in a timeout leaves it borrowed, and
 * the next call that needs the area answers AAR_E_BUSY until the late
 * completion has arrived (spec 7.1, 9.6). All wait the file timeout. */
static int path_request(uint32_t op, const char *path, const uint32_t *args,
			uint32_t nargs, aar_completion *c)
{
	zzapp_xport_buf_t buf;
	uint32_t len = 0U;
	int rc;

	if (!service_rec())
		return AAR_E_IO;
	rc = put_path(path, &buf, &len);
	if (rc != AAR_OK)
		return rc;
	return file_request(op, args, nargs, &buf, len, c);
}

int zzapp_svc_file_delete(const char *path)
{
	return path_request(AAR_OP_FILE_DELETE, path, NULL, 0U, NULL);
}

int zzapp_svc_dir_create(const char *path)
{
	return path_request(AAR_OP_DIR_CREATE, path, NULL, 0U, NULL);
}

int zzapp_svc_file_rename(const char *from, const char *to, uint32_t flags)
{
	zzapp_xport_buf_t buf;
	uint32_t args[1];
	size_t nf;
	size_t nt;
	int rc;

	if (!service_rec())
		return AAR_E_IO;
	if ((flags & ~(uint32_t)AAR_RENAME_REPLACE) != 0U || !from || !to)
		return AAR_E_INVAL;
	nf = strlen(from);
	nt = strlen(to);
	if (nf == 0U || nf > ZZAPP_SVC_PATH_MAX || nt == 0U ||
	    nt > ZZAPP_SVC_PATH_MAX)
		return AAR_E_INVAL;
	rc = zzapp_xport_service_area(service_rec(), ZZAPP_XPORT_AREA_SCRATCH,
				      &buf);
	if (rc != AAR_OK)
		return rc;
	/* `from`, a NUL, `to`, a NUL: at most 512 bytes of the 1 KiB area. */
	memcpy(buf.ptr, from, nf + 1U);
	memcpy((uint8_t *)buf.ptr + nf + 1U, to, nt + 1U);
	args[AAR_A_FILE_RENAME_FLAGS] = flags;
	return file_request(AAR_OP_FILE_RENAME, args, 1U, &buf,
			    (uint32_t)(nf + nt + 2U), NULL);
}

int zzapp_svc_file_set_date(const char *path, int64_t mtime)
{
	uint32_t args[2];

	args[AAR_A_FILE_DATE_HI] = (uint32_t)((uint64_t)mtime >> 32);
	args[AAR_A_FILE_DATE_LO] = (uint32_t)mtime;
	return path_request(AAR_OP_FILE_SET_DATE, path, args, 2U, NULL);
}

int zzapp_svc_file_set_protect(const char *path, uint32_t protect)
{
	uint32_t args[1];

	/* A bit that no AAR_PROT_* names is refused here, before the client. */
	if ((protect & ~0xffU) != 0U)
		return AAR_E_INVAL;
	args[AAR_A_FILE_PROTECT] = protect;
	return path_request(AAR_OP_FILE_SET_PROTECT, path, args, 1U, NULL);
}

int zzapp_svc_file_truncate(int fd, uint64_t size)
{
	struct zzapp_service_runtime *state = current_runtime();
	uint32_t args[3];

	if (!state)
		return AAR_E_IO;
	/* A retired id (timed out) is not live; an id opened without WRITE is
	 * the wrong direction. */
	if (!slot_live(state->file_open, fd) ||
	    (state->file_access[fd_index(fd)] & AAR_FILE_WRITE) == 0U)
		return AAR_E_BADF;
	if (file_async_fd_busy(service_rec(), fd))
		return AAR_E_BUSY;
	args[AAR_A_FILE_TRUNC_FD] = (uint32_t)fd;
	args[AAR_A_FILE_TRUNC_SIZE_HI] = (uint32_t)(size >> 32);
	args[AAR_A_FILE_TRUNC_SIZE_LO] = (uint32_t)size;
	return file_request(AAR_OP_FILE_TRUNCATE, args, 3U, NULL, 0U, NULL);
}

int zzapp_svc_volume_info(const char *path, aar_volume_info *out)
{
	aar_volume_info info;
	aar_completion c;
	uint32_t size;
	uint32_t total;
	uint32_t used;
	uint32_t block;
	int rc;

	if (!out)
		return AAR_E_INVAL;
	size = out->size;
	rc = path_request(AAR_OP_VOLUME_INFO, path, NULL, 0U, &c);
	if (rc != AAR_OK)
		return rc;
	total = c.out[AAR_R_VOLUME_BLOCKS_TOTAL];
	used = c.out[AAR_R_VOLUME_BLOCKS_USED];
	block = c.out[AAR_R_VOLUME_BLOCK_BYTES];
	/* A volume with no block size has nothing to report. */
	if (block == 0U)
		return AAR_E_IO;
	memset(&info, 0, sizeof(info));
	info.size = size < sizeof(info) ? size : (uint32_t)sizeof(info);
	info.block_bytes = block;
	info.total_bytes = (uint64_t)total * block;
	info.free_bytes = used < total ? (uint64_t)(total - used) * block : 0U;
	info.flags = c.out[AAR_R_VOLUME_FLAGS] & AAR_VOLF_READONLY;
	memcpy(out, &info, size < sizeof(info) ? size : sizeof(info));
	return AAR_OK;
}

/* The request carries the path and reserves room for the comment in the
 * same scratch area. A malformed completion must not reach the app buffer. */
int zzapp_svc_file_get_comment(const char *path, void *buf, uint32_t bytes,
			       uint32_t *out_n)
{
	zzapp_xport_buf_t area;
	aar_completion c;
	uint32_t args[1];
	uint32_t len;
	uint32_t cap;
	uint32_t full;
	int rc;

	if (!out_n)
		return AAR_E_INVAL;
	*out_n = 0U;
	if (bytes != 0U && !buf)
		return AAR_E_INVAL;
	if (!service_rec())
		return AAR_E_IO;
	rc = put_path(path, &area, &len);
	if (rc != AAR_OK)
		return rc;
	cap = bytes < AAR_FILE_COMMENT_MAX ? bytes : AAR_FILE_COMMENT_MAX;
	args[AAR_A_FILE_COMMENT_CAP] = cap;
	memset(&c, 0, sizeof(c));
	rc = file_request(AAR_OP_FILE_GET_COMMENT, args, 1U, &area,
			  len > cap ? len : cap, &c);
	if (rc != AAR_OK && rc != AAR_E_NOSPC)
		return rc;
	full = c.out[AAR_R_FILE_COMMENT_LEN];
	if (full > AAR_FILE_COMMENT_MAX ||
	    (rc == AAR_OK ? full > cap : full <= cap))
		return AAR_E_IO;
	len = full < cap ? full : cap;
	if (len != 0U)
		memcpy(buf, area.ptr, len);
	*out_n = full;
	return rc;
}

int zzapp_svc_file_set_comment(const char *path, const void *data,
			       uint32_t bytes)
{
	zzapp_xport_buf_t area;
	uint32_t len;
	int rc;

	_Static_assert(ZZAPP_SVC_PATH_MAX + 1U + AAR_FILE_COMMENT_MAX <=
		       ZZAPP_XPORT_SCRATCH_BYTES, "file comment request fits scratch");
	if (bytes > AAR_FILE_COMMENT_MAX || (bytes != 0U && !data))
		return AAR_E_INVAL;
	if (bytes != 0U && memchr(data, 0, bytes) != NULL)
		return AAR_E_INVAL;
	if (!service_rec())
		return AAR_E_IO;
	rc = put_path(path, &area, &len);
	if (rc != AAR_OK)
		return rc;
	if (bytes != 0U)
		memcpy((uint8_t *)area.ptr + len, data, bytes);
	return file_request(AAR_OP_FILE_SET_COMMENT, NULL, 0U, &area,
			    len + bytes, NULL);
}

/* ---- Time, locale, environment and clipboard (spec 7.3, 7.4) -------------------- */

/* These park the app until the client completes them: no deadline of their
 * own (the spec gives them none), a stop ends the wait with AAR_E_IO, and a
 * client that does not serve one answers AAR_E_NOSYS. */
static int host_request(uint32_t op, uint16_t flags, const uint32_t *args,
			uint32_t nargs, const zzapp_xport_buf_t *buf,
			uint32_t buf_len, aar_completion *c)
{
	return blocking_request_flags(op, flags, ZZAPP_WAIT_FILE, 0U, args,
				      nargs, buf, buf_len, c);
}

/* A staging buffer of `bytes` (1 to ZZAPP_XPORT_DATA_MAX): the scratch area
 * when it is small enough, the data block otherwise. Either answers
 * AAR_E_BUSY while an abandoned request still names it. */
static int stage_buffer(zzapp_rec_t *rec, uint32_t bytes,
			zzapp_xport_buf_t *buf)
{
	if (bytes <= ZZAPP_XPORT_SCRATCH_BYTES)
		return zzapp_xport_service_area(rec, ZZAPP_XPORT_AREA_SCRATCH,
						buf);
	return zzapp_xport_data_reserve(rec, bytes, buf);
}

int zzapp_svc_time_unix_us(int64_t *out_us)
{
	aar_completion c;
	int rc;

	if (!out_us)
		return AAR_E_INVAL;
	if (!service_rec())
		return AAR_E_IO;
	rc = host_request(AAR_OP_TIME_UNIX, 0U, NULL, 0U, NULL, 0U, &c);
	if (rc != AAR_OK)
		return rc;
	*out_us = (int64_t)(((uint64_t)c.out[AAR_R_TIME_UNIX_US_HI] << 32) |
			    c.out[AAR_R_TIME_UNIX_US_LO]);
	return AAR_OK;
}

int zzapp_svc_locale_info(aar_locale_info *out)
{
	zzapp_rec_t *rec = service_rec();
	zzapp_xport_buf_t area;
	aar_locale_info info;
	aar_completion c;
	const uint8_t *r;
	uint32_t size;
	int rc;

	if (!out)
		return AAR_E_INVAL;
	size = out->size;
	if (!rec)
		return AAR_E_IO;
	rc = zzapp_xport_service_area(rec, ZZAPP_XPORT_AREA_SCRATCH, &area);
	if (rc != AAR_OK)
		return rc;
	memset(area.ptr, 0, AAR_LOCALE_REC_BYTES);
	rc = host_request(AAR_OP_LOCALE_INFO, 0U, NULL, 0U, &area,
			  AAR_LOCALE_REC_BYTES, &c);
	if (rc != AAR_OK)
		return rc;
	r = (const uint8_t *)area.ptr;
	memset(&info, 0, sizeof(info));
	info.size = size < sizeof(info) ? size : (uint32_t)sizeof(info);
	info.utc_offset_min =
		(int32_t)zzapp_get_be32(r + AAR_LOCALE_REC_UTC_OFFSET_MIN);
	info.flags = zzapp_get_be32(r + AAR_LOCALE_REC_FLAGS) &
		     AAR_LOCALEF_OFFSET_KNOWN;
	/* Every string field keeps its NUL, whatever the client sent. */
	memcpy(info.language, r + AAR_LOCALE_REC_LANGUAGE,
	       sizeof(info.language) - 1U);
	memcpy(info.country, r + AAR_LOCALE_REC_COUNTRY,
	       sizeof(info.country) - 1U);
	memcpy(info.decimal_point, r + AAR_LOCALE_REC_DECIMAL_POINT,
	       sizeof(info.decimal_point) - 1U);
	memcpy(info.group_separator, r + AAR_LOCALE_REC_GROUP_SEPARATOR,
	       sizeof(info.group_separator) - 1U);
	memcpy(out, &info, size < sizeof(info) ? size : sizeof(info));
	return AAR_OK;
}

/* The request's buffer holds the name, which the client reads first, and the
 * room for the value, which it then writes over it: it is as long as the
 * larger of the two. The first try uses at most the scratch area; the client
 * reports the full length of the value even when it cut it, so a value that
 * did not fit the room is asked for again with room for it and its NUL,
 * and a caller that passes a huge `bytes` for a short value stages nothing
 * big. A value the data block cannot hold (ZZAPP_XPORT_DATA_MAX bytes or
 * more) cannot be moved in one request: it is reported with AAR_E_NOSPC and
 * its full length, as the spec does for a buffer that is too small. */
int zzapp_svc_env_get(const char *name, char *buf, uint32_t bytes,
		      uint32_t *out_n)
{
	zzapp_rec_t *rec = service_rec();
	uint32_t room;
	size_t nl;
	unsigned attempt;

	if (!out_n)
		return AAR_E_INVAL;
	*out_n = 0U;
	if (!name)
		return AAR_E_INVAL;
	for (nl = 0U; nl <= ZZAPP_SVC_PATH_MAX && name[nl] != '\0'; nl++)
		;
	if (nl == 0U || nl > ZZAPP_SVC_PATH_MAX)
		return AAR_E_INVAL;
	if (bytes != 0U && !buf)
		return AAR_E_INVAL;
	if (!rec)
		return AAR_E_IO;
	room = bytes < ZZAPP_XPORT_SCRATCH_BYTES ? bytes :
		ZZAPP_XPORT_SCRATCH_BYTES;
	for (attempt = 0U; attempt < 3U; attempt++) {
		zzapp_xport_buf_t area;
		aar_completion c;
		uint32_t need = room > nl + 1U ? room : (uint32_t)(nl + 1U);
		uint32_t full;
		int rc;

		rc = stage_buffer(rec, need, &area);
		if (rc != AAR_OK)
			return rc;
		memcpy(area.ptr, name, nl + 1U);
		rc = host_request(AAR_OP_ENV_GET, 0U, NULL, 0U, &area, need, &c);
		if (rc != AAR_OK)
			return rc;
		full = c.out[AAR_R_ENV_LEN];
		*out_n = full;
		/* The value and its NUL fit the caller only when bytes > full. */
		if (bytes <= full)
			return AAR_E_NOSPC;
		if (full < need) {
			memcpy(buf, area.ptr, full);
			buf[full] = '\0';
			return AAR_OK;
		}
		if (full >= ZZAPP_XPORT_DATA_MAX)
			return AAR_E_NOSPC;
		room = full + 1U;
	}
	/* The variable kept growing between the requests. */
	return AAR_E_IO;
}

/* A clip is read in chunks of the staging buffer, the offset rising from 0.
 * The client holds one consistent clip from offset 0 on, so every chunk
 * reports the same size; a different one is a broken client. *out_n is the
 * full size of the clip, AAR_E_NOSPC when it did not all fit. */
int zzapp_svc_clip_read(uint32_t unit, uint32_t type, void *buf,
			uint32_t bytes, uint32_t *out_n)
{
	zzapp_rec_t *rec = service_rec();
	uint8_t *dst = (uint8_t *)buf;
	uint32_t got = 0U;
	uint32_t size = 0U;
	int first = 1;

	if (!out_n)
		return AAR_E_INVAL;
	*out_n = 0U;
	if (unit >= AAR_CLIP_UNITS ||
	    (type != AAR_CLIP_TEXT && type != AAR_CLIP_IFF) ||
	    (bytes != 0U && !dst))
		return AAR_E_INVAL;
	if (!rec)
		return AAR_E_IO;
	for (;;) {
		zzapp_xport_buf_t area = { NULL, 0U, 0U, 0U };
		aar_completion c;
		uint32_t args[3];
		uint32_t chunk = bytes - got;
		uint32_t want;
		uint32_t n;
		int rc;

		if (chunk > ZZAPP_XPORT_DATA_MAX)
			chunk = ZZAPP_XPORT_DATA_MAX;
		if (chunk != 0U) {
			rc = stage_buffer(rec, chunk, &area);
			if (rc != AAR_OK)
				return rc;
		}
		args[AAR_A_CLIP_UNIT] = unit;
		args[AAR_A_CLIP_TYPE] = type;
		args[AAR_A_CLIP_OFFSET] = got;
		rc = host_request(AAR_OP_CLIP_READ, 0U, args, 3U,
				  chunk != 0U ? &area : NULL, chunk, &c);
		if (rc != AAR_OK)
			return rc;
		n = c.out[AAR_R_CLIP_READ_COUNT];
		if (n > chunk)
			return AAR_E_IO;
		if (first) {
			size = c.out[AAR_R_CLIP_READ_SIZE];
			first = 0;
		} else if (c.out[AAR_R_CLIP_READ_SIZE] != size) {
			return AAR_E_IO;
		}
		if (n != 0U)
			memcpy(dst + got, area.ptr, n);
		got += n;
		if (got > size)
			return AAR_E_IO;
		want = size < bytes ? size : bytes;
		if (got >= want)
			break;
		/* A client that sends nothing while bytes are left would
		 * keep the app asking for ever. */
		if (n == 0U)
			return AAR_E_IO;
	}
	*out_n = size;
	return size > bytes ? AAR_E_NOSPC : AAR_OK;
}

/* A clip is written in chunks, the offset rising from 0 and the last one
 * flagged AAR_CLIPF_LAST: the client replaces the clip only when that chunk
 * completes, so a sequence cut short (stop, error) changes nothing. An empty
 * clip is one request with no buffer. A client that takes other than the
 * whole chunk breaks the sequence. */
int zzapp_svc_clip_write(uint32_t unit, uint32_t type, const void *data,
			 uint32_t bytes)
{
	zzapp_rec_t *rec = service_rec();
	const uint8_t *src = (const uint8_t *)data;
	uint32_t off = 0U;

	if (unit >= AAR_CLIP_UNITS ||
	    (type != AAR_CLIP_TEXT && type != AAR_CLIP_IFF) ||
	    (bytes != 0U && !src))
		return AAR_E_INVAL;
	if (!rec)
		return AAR_E_IO;
	for (;;) {
		zzapp_xport_buf_t area = { NULL, 0U, 0U, 0U };
		aar_completion c;
		uint32_t args[4];
		uint32_t chunk = bytes - off;
		int last;
		int rc;

		if (chunk > ZZAPP_XPORT_DATA_MAX)
			chunk = ZZAPP_XPORT_DATA_MAX;
		last = off + chunk == bytes;
		if (chunk != 0U) {
			rc = stage_buffer(rec, chunk, &area);
			if (rc != AAR_OK)
				return rc;
			memcpy(area.ptr, src + off, chunk);
		}
		args[AAR_A_CLIP_UNIT] = unit;
		args[AAR_A_CLIP_TYPE] = type;
		args[AAR_A_CLIP_OFFSET] = off;
		args[AAR_A_CLIP_TOTAL] = bytes;
		rc = host_request(AAR_OP_CLIP_WRITE,
				  last ? (uint16_t)AAR_CLIPF_LAST : 0U, args,
				  4U, chunk != 0U ? &area : NULL, chunk, &c);
		if (rc != AAR_OK)
			return rc;
		if (c.out[AAR_R_CLIP_WRITE_COUNT] != chunk)
			return AAR_E_IO;
		off += chunk;
		if (last)
			return AAR_OK;
	}
}

/* A record dropped by the services' rate limit counts with the ones the
 * transport drops (no free slot, SQ full): one counter, which is also what
 * AAR_CB_LOG_DROPPED reads. */
static void log_drop(zzapp_rec_t *rec)
{
	rec->xport.stats.log_dropped++;
	zzapp_xport_cb_put(rec, AAR_CB_LOG_DROPPED, rec->xport.stats.log_dropped);
}

void zzapp_svc_log(const char *msg, uint32_t len)
{
	struct zzapp_service_runtime *state = current_runtime();
	zzapp_rec_t *rec = service_rec();
	uint64_t t;

	if (!rec || !state || !msg || !zzapp_xport_live(rec))
		return;
	if (len > AAR_LOG_MAX)
		len = AAR_LOG_MAX;
	t = now_us();
	if (t - state->log_ring_window >= 1000000ull) {
		state->log_ring_window = t;
		state->log_ring_count = 0U;
	}
	if (state->log_ring_count >= ZZAPP_LOG_RING_HZ) {
		log_drop(rec);
		return;
	}
	/* The transport counts a record it cannot post (no free slot, SQ full)
	 * in AAR_CB_LOG_DROPPED itself. */
	if (len != 0U && zzapp_xport_post_log(rec, msg, len) == AAR_OK)
		state->log_ring_count++;
#ifndef ZZAPP_HOST_TEST
	if (t - state->log_uart_window >= 1000000ull) {
		state->log_uart_window = t;
		state->log_uart_count = 0U;
	}
	if (state->log_uart_count < ZZAPP_LOG_UART_HZ) {
		printf("[app] %.*s\r\n", (int)len, msg);
		state->log_uart_count++;
	}
#endif
}

int zzapp_svc_should_stop(void)
{
	return zzapp_services_stop_requested();
}

/* The input events a display delivers only when it was granted them
 * (spec 8.3): the client sends none otherwise, and one that does is not
 * passed on. */
static int event_gated_out(const struct zzapp_service_runtime *state,
			   uint16_t type)
{
	uint32_t caps = state->display_bound ? state->display_caps : 0U;

	if (type == AAR_EV_RESIZE)
		return (caps & AAR_DISPCAP_RESIZE) == 0U;
	if (type == AAR_EV_TEXT)
		return (caps & AAR_DISPCAP_TEXT_INPUT) == 0U;
	if (type == AAR_EV_CLOSE_REQUEST)
		return (caps & AAR_DISPCAP_CLOSE_REQUEST) == 0U;
	return 0;
}

/* aar_event_poll. Lifecycle events (stop, low memory) are always delivered;
 * the client's input events only with AAR_GROUP_INPUT. A stop request queues
 * one AAR_EV_STOP whether or not the client also posts one. */
int zzapp_svc_event_poll(aar_event *out, uint32_t max, uint32_t *out_n)
{
	zzapp_rec_t *rec = service_rec();
	uint32_t slot = service_pid();
	uint32_t n = 0U;

	if (!out_n)
		return AAR_E_INVAL;
	*out_n = 0U;
	if (max != 0U && !out)
		return AAR_E_INVAL;
	/* The client's stop word is a request: latch it so its event is queued
	 * now and not at the next tick. */
	(void)stop_requested_rec(rec);
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
	if ((zzapp_runtime_groups() & AAR_GROUP_INPUT) == 0U || !rec ||
	    !zzapp_xport_live(rec)) {
		*out_n = n;
		return AAR_OK;
	}
	if (zzapp_xport_pump(rec) < 0) {
		*out_n = n;
		return AAR_E_IO;
	}
	while (n < max && zzapp_xport_take_event(rec, &out[n]) == 1) {
		/* The client's own stop event is the delivery of the request;
		 * a second one for the same request is dropped. A stop event
		 * with no stop requested delivers nothing: the real request
		 * still gets its event. */
		if (out[n].type == AAR_EV_STOP && stop_requested_rec(rec) &&
		    zzapp_host_note_stop_event_delivered &&
		    zzapp_host_note_stop_event_delivered(slot))
			continue;
		if (event_gated_out(runtime_of(rec), out[n].type))
			continue;
		n++;
	}
	*out_n = n;
	return AAR_OK;
}

/* ---- Console ------------------------------------------------------------- */

static int console_pending(zzapp_rec_t *rec)
{
	struct zzapp_service_runtime *state = runtime_of(rec);

	return state->console_head != state->console_tail ||
	       zzapp_xport_req_state(rec, ZZAPP_XPORT_REQ_CONSOLE_OUT) !=
		       ZZAPP_XPORT_REQ_FREE;
}

/*
 * Output goes into a firmware ring (the console-out area of the service
 * block) and out through one outstanding AAR_OP_CONSOLE_WRITE at a time:
 * the completion's COUNT frees ring space, and the next contiguous span is
 * posted. A wrap is two requests. Runs from console_write, from a parked
 * writer's wake test and from the pass for a record with output in flight
 * (also one that has ended: a shell keeps draining until released).
 */
static void console_service(zzapp_rec_t *rec)
{
	struct zzapp_service_runtime *state = runtime_of(rec);
	zzapp_xport_buf_t area;
	aar_completion c;
	aar_request req;
	uint32_t at;
	uint32_t n;
	uint32_t id;

	if (!zzapp_xport_live(rec))
		return;
	if (zzapp_xport_req_state(rec, ZZAPP_XPORT_REQ_CONSOLE_OUT) ==
		    ZZAPP_XPORT_REQ_DONE &&
	    zzapp_xport_take_completion(rec, ZZAPP_XPORT_REQ_CONSOLE_OUT, &c) == 1) {
		uint32_t done = c.result == AAR_OK ?
			c.out[AAR_R_CONSOLE_COUNT] : state->console_inflight;

		/* A client that consumes nothing, or an error, drops the span:
		 * output is not worth a stuck app. */
		if (done == 0U || done > state->console_inflight)
			done = state->console_inflight;
		state->console_tail += done;
		state->console_inflight = 0U;
	}
	if (zzapp_xport_req_state(rec, ZZAPP_XPORT_REQ_CONSOLE_OUT) !=
	    ZZAPP_XPORT_REQ_FREE ||
	    state->console_head == state->console_tail)
		return;
	if (zzapp_xport_service_area(rec, ZZAPP_XPORT_AREA_CONSOLE_OUT, &area) !=
	    AAR_OK)
		return;
	at = state->console_tail % ZZAPP_XPORT_CONSOLE_OUT_BYTES;
	n = ZZAPP_XPORT_CONSOLE_OUT_BYTES - at;
	if (n > state->console_head - state->console_tail)
		n = state->console_head - state->console_tail;
	memset(&req, 0, sizeof(req));
	req.op = AAR_OP_CONSOLE_WRITE;
	req.buf = area.token;
	req.buf_off = area.off + at;
	req.buf_len = n;
	if (zzapp_xport_post_request(rec, ZZAPP_XPORT_REQ_CONSOLE_OUT, &req, 0U,
				     &id) == AAR_OK)
		state->console_inflight = n;
}

int zzapp_svc_console_write(const void *buf, uint32_t len, uint32_t *out_n)
{
	struct zzapp_service_runtime *state = current_runtime();
	zzapp_rec_t *rec = service_rec();
	const uint8_t *src = (const uint8_t *)buf;
	zzapp_xport_buf_t area;
	uint32_t done = 0U;
	int rc;

	if (!out_n || (len != 0U && !src))
		return AAR_E_INVAL;
	*out_n = 0U;
	if (!rec || !zzapp_xport_live(rec))
		return AAR_E_IO;
	cbf_or_current(AAR_CBF_CONSOLE);
	rc = zzapp_xport_service_area(rec, ZZAPP_XPORT_AREA_CONSOLE_OUT, &area);
	if (rc != AAR_OK)
		return rc;
	slot_state_lock_acquire();
	set_add(&set_svc, service_pid());
	slot_state_lock_release();
	while (done < len) {
		uint32_t used = state->console_head - state->console_tail;
		uint32_t at;
		uint32_t n;

		if (used >= ZZAPP_XPORT_CONSOLE_OUT_BYTES) {
			/* Full: let completions free space, else park. */
			if (zzapp_xport_pump(rec) < 0) {
				wait_end();
				return AAR_E_IO;
			}
			console_service(rec);
			if (state->console_head - state->console_tail <
			    ZZAPP_XPORT_CONSOLE_OUT_BYTES)
				continue;
			if (zzapp_services_stop_requested()) {
				wait_end();
				return AAR_E_IO;
			}
			wait_begin(ZZAPP_WAIT_CONSOLE_WRITE, state->console_tail, 0U);
			yield_wait_once();
			if (wait_status() != AAR_OK) {
				wait_end();
				return wait_status();
			}
			continue;
		}
		at = state->console_head % ZZAPP_XPORT_CONSOLE_OUT_BYTES;
		n = ZZAPP_XPORT_CONSOLE_OUT_BYTES - at;
		if (n > ZZAPP_XPORT_CONSOLE_OUT_BYTES - used)
			n = ZZAPP_XPORT_CONSOLE_OUT_BYTES - used;
		if (n > len - done)
			n = len - done;
		memcpy((uint8_t *)area.ptr + at, src + done, n);
		state->console_head += n;
		done += n;
	}
	wait_end();
	console_service(rec);
	*out_n = done;
	return AAR_OK;
}

/* Input is a request the client answers while it is outstanding, so it reads
 * the keyboard only when the app asks. COUNT 0 is the end of input. */
int zzapp_svc_console_read(void *buf, uint32_t len, uint32_t *out_n)
{
	zzapp_rec_t *rec = service_rec();
	uint8_t *dst = (uint8_t *)buf;
	zzapp_xport_buf_t area;
	aar_completion c;
	uint32_t room;
	int rc;

	if (!out_n || (len != 0U && !dst))
		return AAR_E_INVAL;
	*out_n = 0U;
	if (!rec || !zzapp_xport_live(rec))
		return AAR_E_IO;
	cbf_or_current(AAR_CBF_CONSOLE);
	if (len == 0U)
		return AAR_OK;
	rc = zzapp_xport_service_area(rec, ZZAPP_XPORT_AREA_CONSOLE_IN, &area);
	if (rc != AAR_OK)
		return rc;
	room = len < area.bytes ? len : area.bytes;
	rc = blocking_request(AAR_OP_CONSOLE_READ, ZZAPP_WAIT_CONSOLE_READ, 0U,
			      NULL, 0U, &area, room, &c);
	if (rc != AAR_OK)
		return rc;
	if (c.out[AAR_R_CONSOLE_COUNT] > room)
		return AAR_E_IO;
	memcpy(dst, area.ptr, c.out[AAR_R_CONSOLE_COUNT]);
	*out_n = c.out[AAR_R_CONSOLE_COUNT];
	return AAR_OK;
}

/* ---- Messages ------------------------------------------------------------ */

int zzapp_svc_msg_send(const void *msg)
{
	zzapp_rec_t *rec = service_rec();

	if (!msg)
		return AAR_E_INVAL;
	if (!rec || !zzapp_xport_live(rec))
		return AAR_E_IO;
	return zzapp_xport_post_msg(rec, msg);
}

int zzapp_svc_msg_poll(void *msg)
{
	zzapp_rec_t *rec = service_rec();

	if (!msg)
		return AAR_E_INVAL;
	if (!rec || !zzapp_xport_live(rec))
		return AAR_E_IO;
	if (zzapp_xport_pump(rec) < 0)
		return AAR_E_IO;
	return zzapp_xport_take_msg(rec, msg);
}

/*
 * A queued message ends the wait before anything else, so a stop never
 * drops one. The wait is level-triggered: it pumps and re-tests the inbox
 * after every resume, and the service pass wakes any parked EVENT waiter
 * whose inbox is not empty, so a message that lands between the check and the
 * park cannot be missed.
 */
int zzapp_svc_msg_wait(uint32_t timeout_us)
{
	zzapp_rec_t *rec = service_rec();
	uint64_t deadline = 0U;
	int resume_status = AAR_OK;

	if (!rec || !zzapp_xport_live(rec))
		return AAR_E_IO;
	if (timeout_us != 0U)
		deadline = now_us() + timeout_us;
	for (;;) {
		if (zzapp_xport_pump(rec) < 0) {
			wait_end();
			return AAR_E_IO;
		}
		if (zzapp_xport_msg_pending(rec) != 0U) {
			wait_end();
			return AAR_OK;
		}
		/* The inbox is read before the wake reason is honoured, so a
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
		wait_begin(ZZAPP_WAIT_EVENT, 0U, deadline);
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
	struct zzapp_service_runtime *state = current_runtime();
	uint32_t ack;

	if (!state->display_bound)
		return;
	if (display_mailbox_enabled())
		return;
	ack = __atomic_load_n(&state->display_flip_ack, __ATOMIC_ACQUIRE);
	if (ack == state->display_ack_seen)
		return;
	state->display_ack_seen = ack;
	if (state->display_flip_result < 0) {
		/* The client did not change the scanout: the front stays and
		 * the page that was queued for it goes back to the spare. */
		if (state->display_queued != 0U) {
			state->display_spare = state->display_queued;
			state->display_queued = 0U;
		}
		return;
	}
	if (state->display_queued != 0U) {
		uint32_t front = state->display_front;

		state->display_front = state->display_queued;
		state->display_spare = front;
		state->display_queued = 0U;
	} else if (!state->display_back_owned && state->display_back != 0U) {
		uint32_t front = state->display_front;

		state->display_front = state->display_back;
		state->display_back = front;
	}
}

static uint32_t token_address(uint32_t token);

/* Caller holds g_slot_state_lock. The frame the app last handed over is now
 * known to be shown: by the client's completion, by the scanout reaching the
 * address the client submitted, or by the card latching a LATEST frame. A
 * timeout that merely lets the app go is none of these and does not call
 * this. 0 means "none yet", so a clock that reads 0 is nudged. */
static uint64_t display_note_present_locked(struct zzapp_service_runtime *state,
					uint64_t frame_id)
{
	uint64_t t = now_us();

	state->display_present_us = t != 0U ? t : 1U;
	if (frame_id)
		aar_present_finish(&state->present_feedback, frame_id,
				   AAR_PRESENT_ACKNOWLEDGED,
				   AAR_PRESENT_REASON_NONE, t);
	return t;
}

static int present_pending_locked(const struct zzapp_service_runtime *state,
				  uint64_t frame_id)
{
	const aar_present_queue *queue = &state->present_feedback;
	unsigned slot = (unsigned)frame_id & AAR_PRESENT_QUEUE_MASK;

	return (queue->pending & (1U << slot)) != 0U &&
	       queue->frames[slot].frame_id == frame_id;
}

/* Caller holds g_slot_state_lock. Clear the association before releasing a
 * flip, so a late blank or completion cannot resolve the next ticket. */
static void flip_unconfirmed_locked(struct zzapp_service_runtime *state,
				    uint32_t reason, uint64_t completed_us)
{
	aar_present_finish(&state->present_feedback, state->display_flip_frame,
			   AAR_PRESENT_UNCONFIRMED, reason, completed_us);
	state->display_flip_frame = 0U;
}

static uint32_t present_error_reason(int status)
{
	return status == AAR_E_TIMEDOUT ? AAR_PRESENT_REASON_TIMEOUT :
					AAR_PRESENT_REASON_BACKEND;
}

static void display_cancel_pending_locked(zzapp_rec_t *rec)
{
	struct zzapp_service_runtime *state = runtime_of(rec);

	if (state->present_feedback.pending)
		aar_present_cancel(&state->present_feedback, now_us());
	if (state->display_flip_frame) {
		state->display_flip_frame = 0U;
		zzapp_xport_abandon(rec, ZZAPP_XPORT_REQ_FLIP);
		state->display_flip_result = AAR_E_IO;
		__atomic_store_n(&state->display_flip_ack, state->display_flip_gen,
				 __ATOMIC_RELEASE);
	}
}

static void display_cancel_on_stop_locked(zzapp_rec_t *rec)
{
	struct zzapp_service_runtime *state = runtime_of(rec);

	if (state->present_feedback.pending && !state->present_submitting &&
	    stop_requested_rec(rec))
		display_cancel_pending_locked(rec);
}

/* A submitted flip whose scanout acknowledgement does not come (the address
 * the client says it shows never reaches scanout) is complete after this
 * long: the app is never held by it. It replaces the client's own fallback. */
#define FLIP_ACK_TIMEOUT_US 100000U
/* Feedback can expire while a live client has not answered, or a LATEST
 * frame has not latched. Neither timeout gives the app a buffer back. */
#define PRESENT_FEEDBACK_TIMEOUT_US 30000000ull

/* Caller holds g_slot_state_lock. */
static void flip_expire_locked(struct zzapp_service_runtime *state)
{
	uint32_t gen = state->display_flip_gen;
	uint64_t t;

	if (gen == 0U || state->display_flip_submit != gen ||
	    state->display_flip_ack == gen)
		return;
	t = now_us();
	if (t - state->display_flip_submit_us < FLIP_ACK_TIMEOUT_US)
		return;
	if (state->display_flip_frame)
		flip_unconfirmed_locked(state, AAR_PRESENT_REASON_TIMEOUT, t);
	__atomic_store_n(&state->display_flip_ack, gen, __ATOMIC_RELEASE);
}

/*
 * The flip request's completion, when it is in. Caller holds
 * g_slot_state_lock: core 0's vblank ISR reads the words this writes. A
 * client that submitted the pan or buffer change and relies on the card's
 * vertical blank says so (ZZAPP_FLIPF_SUBMITTED) and names the page it now
 * shows (AAR_R_DISPLAY_FLIP_FRONT): the ISR acknowledges when the scanout
 * reaches that address, or the timeout does. Without SUBMITTED the flip is
 * complete now.
 */
static void flip_apply_locked(zzapp_rec_t *rec, int pump_rc)
{
	struct zzapp_service_runtime *state = runtime_of(rec);
	int req_state = zzapp_xport_req_state(rec, ZZAPP_XPORT_REQ_FLIP);
	aar_completion c;

	flip_expire_locked(state);
	if (req_state == ZZAPP_XPORT_REQ_POSTED && state->display_flip_frame) {
		uint64_t t = now_us();

		if (pump_rc < 0) {
			zzapp_xport_abandon(rec, ZZAPP_XPORT_REQ_FLIP);
			state->display_flip_result = AAR_E_IO;
			flip_unconfirmed_locked(state, AAR_PRESENT_REASON_BACKEND, t);
			__atomic_store_n(&state->display_flip_ack,
					 state->display_flip_gen, __ATOMIC_RELEASE);
		} else if (t - state->display_flip_submit_us >= PRESENT_FEEDBACK_TIMEOUT_US) {
			/* The late reply still owns the buffer and can acknowledge
			 * the flip, but must not resolve its feedback a second time. */
			state->display_reply_expired = state->display_flip_gen;
			flip_unconfirmed_locked(state, AAR_PRESENT_REASON_TIMEOUT, t);
		}
		return;
	}
	if (req_state != ZZAPP_XPORT_REQ_DONE ||
	    zzapp_xport_take_completion(rec, ZZAPP_XPORT_REQ_FLIP, &c) != 1)
		return;
	state->display_flip_result = c.result < 0 ? c.result : AAR_OK;
	if (c.result == AAR_OK && (c.flags & ZZAPP_FLIPF_SUBMITTED) != 0U &&
	    state->display_scanout_ack) {
		uint32_t shown = token_address(c.out[AAR_R_DISPLAY_FLIP_FRONT]);
		uint32_t i;

		for (i = 0U; shown != 0U && i < 3U; i++) {
			if (state->display_alias_phys[i] == shown) {
				state->display_flip_address = shown;
				break;
			}
		}
		state->display_flip_submit_us = now_us();
		__atomic_store_n(&state->display_flip_submit,
				 state->display_flip_gen, __ATOMIC_RELEASE);
	} else {
		if (c.result == AAR_OK) {
			int submitted = (c.flags & ZZAPP_FLIPF_SUBMITTED) != 0U;
			uint64_t t = display_note_present_locked(state,
						submitted ? 0U : state->display_flip_frame);

			/* Keep the legacy timestamp and release for this reply.
			 * Without scanout feedback, a pan submission alone does
			 * not acknowledge the tracked frame. */
			if (submitted && state->display_flip_frame)
				flip_unconfirmed_locked(state, AAR_PRESENT_REASON_BACKEND, t);
			state->display_flip_frame = 0U;
		} else if (state->display_flip_frame) {
			flip_unconfirmed_locked(state, present_error_reason(c.result),
						now_us());
		}
		__atomic_store_n(&state->display_flip_ack,
				 state->display_flip_gen, __ATOMIC_RELEASE);
	}
}

/* The app thread: the pump needs no lock, the words do. */
static void flip_service(zzapp_rec_t *rec)
{
	int pump_rc = 0;

	if (zzapp_xport_req_state(rec, ZZAPP_XPORT_REQ_FLIP) ==
	    ZZAPP_XPORT_REQ_POSTED)
		pump_rc = zzapp_xport_pump(rec);
	slot_state_lock_acquire();
	flip_apply_locked(rec, pump_rc);
	slot_state_lock_release();
}

int zzapp_display_flip_idle(void)
{
	struct zzapp_service_runtime *state = current_runtime();

	if (!state->display_bound)
		return 0;
	if (display_mailbox_enabled())
		return 1;
	flip_service(service_rec());
	display_reap_flip();
	return state->display_flip_gen == state->display_ack_seen;
}

/* The core-1 tick only checks displays with a tracked frame pending. Take
 * the locks in the same order as native latch and replacement. */
static void display_mailbox_expire(struct zzapp_service_runtime *state)
{
	display_mailbox_lock_acquire_for(state);
	if (state->display_mailbox_frame) {
		uint64_t t = now_us();

		if (t - state->display_mailbox_started_us >= PRESENT_FEEDBACK_TIMEOUT_US) {
			slot_state_lock_acquire();
			aar_present_finish(&state->present_feedback, state->display_mailbox_frame,
					   AAR_PRESENT_UNCONFIRMED, AAR_PRESENT_REASON_TIMEOUT, t);
			state->display_mailbox_frame = 0U;
			slot_state_lock_release();
		}
	}
	display_mailbox_lock_release_for(state);
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
	uint32_t i;

	if (!scanout)
		return 0;
	for (i = 0U; ; i++) {
		uint32_t pid;
		zzapp_rec_t *rec;
		struct zzapp_service_runtime *state;

		/* The set changes under g_slot_state_lock; the record it names is
		 * type stable and its mailbox is switched off under its own lock
		 * before it is freed, which the checks below take again. */
		slot_state_lock_acquire();
		if (i >= set_display.count) {
			slot_state_lock_release();
			break;
		}
		pid = set_display.pids[i];
		rec = zzapp_rec_by_pid(pid);
		slot_state_lock_release();
		if (!rec)
			continue;
		state = runtime_of(rec);
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
		display_mailbox_pid = pid;
		display_mailbox_state = state;
		return 1;
	}
	return 0;
}

void zzapp_services_display_mailbox_end(int latched)
{
	struct zzapp_service_runtime *state = display_mailbox_state;
	int woke = 0;

	if (display_mailbox_pid == ZZAPP_PID_NONE || !state)
		return;
	if (latched && state->display_mailbox_latch_pending) {
		uint32_t old_shown = state->display_mailbox_shown;

		state->display_mailbox_shown = state->display_mailbox_pending;
		state->display_mailbox_pending = 0U;
		state->display_front = state->display_mailbox_shown;
		state->display_spare = old_shown;
		slot_state_lock_acquire();
		display_note_present_locked(state, state->display_mailbox_frame);
		slot_state_lock_release();
		state->display_mailbox_frame = 0U;
	}
	/* VDMA now scans the address mailbox_begin matched to this record's
	 * buffers: that is a blank of its screen, whether or not a frame was
	 * latched. A latch that failed is not one. Nothing is done for a
	 * record nobody waits on. */
	if (latched && __atomic_load_n(&state->vblank_waiting, __ATOMIC_ACQUIRE)) {
		slot_state_lock_acquire();
		if (state->vblank_waiting) {
			__atomic_store_n(&state->vblank_seq, state->vblank_seq + 1U,
					 __ATOMIC_RELEASE);
			woke = 1;
		}
		slot_state_lock_release();
	}
	state->display_mailbox_latch_pending = 0;
	display_mailbox_pid = ZZAPP_PID_NONE;
	display_mailbox_state = NULL;
	display_mailbox_lock_release_for(state);
	if (woke)
		ring_core1();
}

uint32_t zzapp_svc_display_latest_dropped(void)
{
	if (!display_mailbox_enabled())
		return 0U;
	return current_runtime()->display_mailbox_replaced;
}

int zzapp_display_latest(void)
{
	struct zzapp_service_runtime *state = current_runtime();

	return state && display_mailbox_enabled();
}

/* 1 when `address` is one of the buffers the record's display shows or has
 * submitted: the screen the card scans out is this record's. */
static int display_scanout_owned(const struct zzapp_service_runtime *state,
				 uint32_t address)
{
	uint32_t i;

	if (address == 0U)
		return 0;
	for (i = 0U; i < 3U; i++) {
		if (state->display_alias_phys[i] == address)
			return 1;
	}
	return address == state->display_flip_address;
}

/* 1 when the display is composed by the client (WINDOW or SCALED). */
static int display_client_owned(const struct zzapp_service_runtime *state)
{
	return (state->display_caps & (AAR_DISPCAP_WINDOW | AAR_DISPCAP_SCALED)) != 0U;
}

/* 1 when a blank that scanned out `address` is a blank of the screen the
 * record's waiter is waiting on. A client-owned display's own buffers are
 * never scanned out: only the P96 screen the client named counts, and only by
 * exact address. */
static int display_blank_owned(const struct zzapp_service_runtime *state,
			       uint32_t address)
{
	if (address == 0U)
		return 0;
	if (display_client_owned(state))
		return state->display_source != 0U && address == state->display_source;
	return display_scanout_owned(state, address);
}

/* One record of the vblank pass; the caller holds g_slot_state_lock. 1 when
 * it acknowledged the flip a parked app may be waiting for, or counted a
 * blank for an app parked on one. */
static int display_vblank_locked(zzapp_rec_t *rec, uint32_t scanout_address)
{
	struct zzapp_service_runtime *state = runtime_of(rec);
	uint32_t gen;
	int woke = 0;

	if (!state->display_bound ||
	    __atomic_load_n(&state->display_mailbox_active, __ATOMIC_ACQUIRE))
		return 0;
	/* A native blank waiter is registered only by a direct-scanout display
	 * or a client-owned one with a named screen, and only a blank of that
	 * screen counts. */
	if (state->vblank_waiting && display_blank_owned(state, scanout_address)) {
		__atomic_store_n(&state->vblank_seq, state->vblank_seq + 1U,
				 __ATOMIC_RELEASE);
		woke = 1;
	}
	if (!state->display_scanout_ack)
		return woke;
	gen = state->display_flip_gen;
	if (gen == __atomic_load_n(&state->display_flip_ack, __ATOMIC_ACQUIRE) ||
	    gen != __atomic_load_n(&state->display_flip_submit, __ATOMIC_ACQUIRE) ||
	    scanout_address != state->display_flip_address)
		return woke;
	display_note_present_locked(state, state->display_flip_frame);
	state->display_flip_frame = 0U;
	__atomic_store_n(&state->display_flip_ack, gen, __ATOMIC_RELEASE);
	return 1;
}

/* Core 0, from the vblank ISR. The lock masks this core's IRQs and is
 * recursive, so the ISR may take it over core 0 code that holds it. An app
 * parked on the flip is woken by core 1's pass, and the card's vblank is the
 * one thing that ends that wait without a KICK: with another app on the
 * core the pass would come at the next tick, a frame late for a client that
 * is synchronised to the vblank. So an acknowledgement rings the doorbell. */
void zzapp_services_display_vblank(uint32_t scanout_address)
{
	uint32_t i;
	int acked = 0;

	for (i = set_display.count; i-- > 0U;) {
		zzapp_rec_t *rec;

		slot_state_lock_acquire();
		if (i < set_display.count) {
			rec = zzapp_rec_by_pid(set_display.pids[i]);
			if (rec) {
				SERVICE_LOOKUP_GAP(set_display.pids[i]);
				acked |= display_vblank_locked(rec,
							       scanout_address);
			}
		}
		slot_state_lock_release();
	}
	if (acked)
		ring_core1();
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

uint32_t zzapp_display_id(void)
{
	struct zzapp_service_runtime *state = current_runtime();

	return state && state->display_bound ? state->display_id : 0U;
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
	uint32_t caps = AAR_DISPCAP_ASYNC | AAR_DISPCAP_PRESENT_FEEDBACK;

	if (!out || !current_runtime()->display_bound)
		return AAR_E_INVAL;
	if (zzapp_display_bound(&w, &h, &pitch, &format, &front, &back) != AAR_OK)
		return AAR_E_INVAL;
	front_alias = display_alias(front);
	back_alias = display_alias(back);
	if (display_mailbox_enabled())
		caps |= AAR_DISPCAP_LATEST;
	if (service_rec()->claim.client_caps & ZZAPP_CLIENT_CLUT8)
		caps |= AAR_DISPCAP_CLUT8;
	caps |= current_runtime()->display_caps;
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

/* A flip normally completes within a few VBlanks; one line per wait that
 * outlasts this says which half of the handshake is late. */
#define FLIP_WAIT_REPORT_US 50000U

#ifdef ZZAPP_HOST_TEST
static uint32_t flip_wait_reports;
#endif

/* submit != gen: the client has not shown the frame yet. submit == gen but no
 * ack: the VBlank has not seen the flip address on scanout. */
static void report_slow_flip(uint32_t gen, uint64_t waited_us)
{
#ifndef ZZAPP_HOST_TEST
	struct zzapp_service_runtime *state = current_runtime();

	printf("[app] flip wait %lu ms: gen=%lu ack=%lu submit=%lu addr=%08lx queued=%08lx\r\n",
	       (unsigned long)(waited_us / 1000U), (unsigned long)gen,
	       (unsigned long)state->display_flip_ack,
	       (unsigned long)state->display_flip_submit,
	       (unsigned long)state->display_flip_address,
	       (unsigned long)state->display_queued);
#else
	(void)gen;
	(void)waited_us;
	flip_wait_reports++;
#endif
}

/* A short bounded poll before parking: when the completion is already in the
 * CQ and the VBlank ack follows at once - the common case for an app that
 * presents triple-buffered frames - the wait completes with no scheduler
 * round trip at all. The pump of a posted flip is a read of one index. */
#define FLIP_POLL_ACKS 8U

/* The failure of the last flip, once: a wait that finds the flip complete
 * reports it to its caller and then the slate is clean. */
static int flip_take_result(struct zzapp_service_runtime *state)
{
	int32_t result = state->display_flip_result;

	state->display_flip_result = AAR_OK;
	return result;
}

static int flip_feedback_wait_done(const struct zzapp_service_runtime *state,
				   uint32_t gen)
{
	return state->display_reply_expired == gen || state->display_flip_gen != gen;
}

static int wait_flip(uint32_t gen, int feedback)
{
	struct zzapp_service_runtime *state = current_runtime();
	zzapp_rec_t *rec = service_rec();
	uint64_t started = now_us();
	int reported = 0;
	uint32_t poll;

	for (poll = 0U; poll < FLIP_POLL_ACKS; poll++) {
		if (zzapp_services_stop_requested())
			return AAR_E_IO;
		flip_service(rec);
		if (__atomic_load_n(&state->display_flip_ack,
				    __ATOMIC_ACQUIRE) == gen) {
			display_reap_flip();
			return flip_take_result(state);
		}
		if (feedback && flip_feedback_wait_done(state, gen))
			return AAR_E_TIMEDOUT;
	}
	for (;;) {
		int status;

		if (zzapp_services_stop_requested()) {
			zzapp_xport_abandon(rec, ZZAPP_XPORT_REQ_FLIP);
			wait_end();
			return AAR_E_IO;
		}
		flip_service(rec);
		if (__atomic_load_n(&state->display_flip_ack,
				    __ATOMIC_ACQUIRE) == gen) {
			display_reap_flip();
			wait_end();
			return flip_take_result(state);
		}
		if (feedback && flip_feedback_wait_done(state, gen)) {
			wait_end();
			return AAR_E_TIMEDOUT;
		}
		/* The first registration wakes this parked app at 50 ms solely for
		 * diagnostics. Then it wakes on ownership release, or when a
		 * tracked caller can collect a client reply timeout. */
		wait_begin(ZZAPP_WAIT_FLIP, gen,
			   reported ? 0U : started + FLIP_WAIT_REPORT_US);
		if (feedback)
			state->display_wait_feedback = 1;
		yield_wait_once();
		if (feedback)
			state->display_wait_feedback = 0;
		status = wait_status();
		if (status == AAR_E_TIMEDOUT && !reported) {
			reported = 1;
			wait_end();
			report_slow_flip(gen, now_us() - started);
			continue;
		}
		if (status != AAR_OK) {
			zzapp_xport_abandon(rec, ZZAPP_XPORT_REQ_FLIP);
			wait_end();
			return status;
		}
	}
}

int zzapp_display_wait_flip_idle(void)
{
	struct zzapp_service_runtime *state = current_runtime();

	if (!state || !state->display_bound)
		return AAR_E_INVAL;
	if (display_mailbox_enabled())
		return AAR_OK;
	/* MiniGL may render into the old front as soon as this wait returns.
	 * A feedback timeout does not return ownership. */
	return wait_flip(state->display_flip_gen, 0);
}

/* A token of the client's memory is a board offset; the card address is the
 * token plus the adjust the transport was told. */
static uint32_t token_address(uint32_t token)
{
	return token != 0U ? token + zzapp_xport_env()->token_adjust : 0U;
}

/* The feature caps (AAR_DISPCAP_WINDOW .. SCALED) of a reply: only what the
 * open asked for and the client says it did. A resizable window and its close
 * gadget belong to a window, so they need the window to be granted. */
static uint32_t display_feature_caps(uint32_t flags_req, uint32_t reply)
{
	uint32_t caps = 0U;

	if ((flags_req & AAR_DISPLAY_WINDOW) != 0U &&
	    (reply & AAR_DISPCAP_WINDOW) != 0U) {
		caps |= AAR_DISPCAP_WINDOW;
		if ((flags_req & AAR_DISPLAY_RESIZABLE) != 0U &&
		    (reply & AAR_DISPCAP_RESIZE) != 0U)
			caps |= AAR_DISPCAP_RESIZE;
		if ((flags_req & AAR_DISPLAY_CLOSE_REQUEST) != 0U &&
		    (reply & AAR_DISPCAP_CLOSE_REQUEST) != 0U)
			caps |= AAR_DISPCAP_CLOSE_REQUEST;
	}
	if ((flags_req & AAR_DISPLAY_POINTER_ABS) != 0U &&
	    (reply & AAR_DISPCAP_POINTER_ABS) != 0U)
		caps |= AAR_DISPCAP_POINTER_ABS;
	if ((flags_req & AAR_DISPLAY_TEXT_INPUT) != 0U &&
	    (reply & AAR_DISPCAP_TEXT_INPUT) != 0U)
		caps |= AAR_DISPCAP_TEXT_INPUT;
	if ((flags_req & AAR_DISPLAY_SCALE) != 0U &&
	    (reply & AAR_DISPCAP_SCALED) != 0U)
		caps |= AAR_DISPCAP_SCALED;
	return caps;
}

/*
 * The completion of AAR_OP_DISPLAY_OPEN: the client opened its screen and
 * names its bitmaps. Validate them, map them into the app, lease the range
 * from the kernel and publish the surface (the old display bind, now run by
 * the app thread that asked).
 */
static int display_bind(zzapp_rec_t *rec, uint32_t flags_req,
			const aar_completion *c, int resize)
{
	struct zzapp_service_runtime *state = runtime_of(rec);
	uint32_t width = state->display_req_w;
	uint32_t height = state->display_req_h;
	uint32_t format = state->display_req_format;
	uint32_t pitch = c->out[AAR_R_DISPLAY_OPEN_PITCH];
	uint32_t layout = c->out[AAR_R_DISPLAY_OPEN_LAYOUT];
	uint32_t front = token_address(c->out[AAR_R_DISPLAY_OPEN_FRONT]);
	uint32_t back = token_address(c->out[AAR_R_DISPLAY_OPEN_BACK]);
	uint32_t spare = token_address(c->out[AAR_R_DISPLAY_OPEN_SPARE]);
	uint32_t bpp = format_bpp(format);
	uint32_t bytes;
	uint32_t caps;
	uint32_t source = 0U;
	int client_owned;
	int rc;

	if (state->display_bound && !resize)
		return AAR_E_BUSY;
	/* A window or a scaled surface is composed by the client: the card never
	 * scans it out directly, so there is no scanout acknowledgement to wait
	 * for and no card-owned (LATEST) presentation. With the client's
	 * AAR_DISP_REPLY_SCANOUT, SPARE names the P96 screen that owns the
	 * window instead of an app buffer: it is only compared with the ISR's
	 * scanout address, so it must lie in the P96 window to be used, and a
	 * name that does not degrades to the client-delegated wait. A resize
	 * keeps the screen the open named. */
	caps = display_feature_caps(flags_req, c->flags);
	client_owned = (caps & (AAR_DISPCAP_WINDOW | AAR_DISPCAP_SCALED)) != 0U;
	if (client_owned && resize)
		source = state->display_source;
	if (client_owned && (c->flags & AAR_DISP_REPLY_SCANOUT) != 0U) {
		if (!resize && spare >= ZZAPP_P96_LO && spare < ZZAPP_P96_HI)
			source = spare;
		spare = 0U;
	}
	if (layout == 0U)
		layout = default_layout(format);
	if (!layout_matches_format(format, layout))
		return AAR_E_INVAL;
	if (bpp == 0U || pitch < width * bpp || height > (0xffffffffU / pitch))
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
		state->display_back_owned = 1;
	} else if (!span_ok(back, bytes)) {
		return AAR_E_INVAL;
	} else {
		state->display_back_owned = 0;
	}
	if (source == front || source == back)
		source = 0U;
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
		if (zzapp_kernel_surface_bind(zzapp_runtime_kernel(), service_pid(),
					      zzapp_services_generation(), lease_base,
					      lease_bytes) != 0) {
			revoke_surface_grants();
			release_unbound_back(back);
			return AAR_E_BUSY;
		}
	}

	state->display_width = width;
	state->display_height = height;
	state->display_pitch = pitch;
	state->display_format = format;
	state->display_layout = layout;
	state->display_bytes = bytes;
	state->display_front = front;
	state->display_back = back;
	state->display_spare = spare;
	state->display_client_id = c->out[AAR_R_DISPLAY_OPEN_ID];
	state->display_queued = 0U;
	slot_state_lock_acquire();
	state->display_flip_gen = 0U;
	state->display_flip_ack = 0U;
	state->display_flip_submit = 0U;
	state->display_reply_expired = 0U;
	state->display_flip_address = 0U;
	state->display_ack_seen = 0U;
	state->display_mailbox_replaced = 0U;
	state->display_scanout_ack =
		(rec->claim.client_caps & ZZAPP_CLIENT_SCANOUT_ACK) != 0U &&
		!client_owned;
	state->display_flags = flags_req;
	state->display_caps = caps;
	state->display_source = source;
	if (!resize)
		memset(state->palette_set, 0, sizeof(state->palette_set));
	state->display_id = 1U;
	state->display_bound = 1;
	set_add(&set_display, rec->hdr.pid);
	slot_state_lock_release();
	display_mailbox_disable();
	if ((flags_req & AAR_DISPLAY_LATEST) != 0U && !client_owned &&
	    (rec->claim.client_caps & ZZAPP_CLIENT_LATEST) != 0U &&
	    !state->display_back_owned && spare != 0U) {
		display_mailbox_lock_acquire();
		state->display_mailbox_shown = front;
		state->display_mailbox_pending = 0U;
		state->display_mailbox_buffers[0] = front;
		state->display_mailbox_buffers[1] = back;
		state->display_mailbox_buffers[2] = spare;
		state->display_mailbox_bytes = bytes;
		state->display_mailbox_latch_pending = 0;
		__atomic_store_n(&state->display_mailbox_active, 1, __ATOMIC_RELEASE);
		display_mailbox_lock_release();
	}
	barrier();
	cbf_or_current(AAR_CBF_DISPLAY);
	return AAR_OK;
}

int zzapp_svc_display_open(uint32_t w, uint32_t h, uint32_t format,
			   uint32_t flags, aar_surface *out)
{
	struct zzapp_service_runtime *state = current_runtime();
	zzapp_rec_t *rec = service_rec();
	aar_completion c;
	uint32_t args[4];
	uint32_t bpp;
	int rc;

	if (!out || !rec)
		return AAR_E_INVAL;
	if ((flags & ~ZZAPP_DISPLAY_FLAGS_ALL) != 0U)
		return AAR_E_INVAL;
	bpp = format_bpp(format);
	if (w == 0U || h == 0U || w > AAR_DISPLAY_MAX_DIM ||
	    h > AAR_DISPLAY_MAX_DIM || bpp == 0U)
		return AAR_E_INVAL;
	if (format == AAR_FMT_CLUT8 &&
	    (rec->claim.client_caps & ZZAPP_CLIENT_CLUT8) == 0U)
		return AAR_E_NOSYS;
	if (state->display_bound)
		return AAR_E_BUSY;
	state->display_req_w = w;
	state->display_req_h = h;
	state->display_req_format = format;
	args[AAR_A_DISPLAY_OPEN_W] = w;
	args[AAR_A_DISPLAY_OPEN_H] = h;
	args[AAR_A_DISPLAY_OPEN_FORMAT] = format;
	args[AAR_A_DISPLAY_OPEN_FLAGS] = flags;
	rc = blocking_request(AAR_OP_DISPLAY_OPEN, ZZAPP_WAIT_DISPLAY_OPEN, 0U,
			      args, 4U, NULL, 0U, &c);
	if (rc != AAR_OK)
		return rc;
	rc = display_bind(rec, flags, &c, 0);
	if (rc != AAR_OK) {
		/* The client's screen is of no use to anyone: tell it. */
		uint32_t id = c.out[AAR_R_DISPLAY_OPEN_ID];

		(void)blocking_request(AAR_OP_DISPLAY_CLOSE,
				       ZZAPP_WAIT_DISPLAY_OPEN, 0U, &id, 1U,
				       NULL, 0U, NULL);
		return rc;
	}
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
#else
	(void)card;
	(void)bytes;
#endif
}

/* Host recorder for display range invalidates (CPU reads of what RasterIX
 * wrote straight into the display's buffers). */
static void record_display_invalidate(uint32_t address, uint32_t bytes)
{
#ifdef ZZAPP_HOST_TEST
	zzapp_test_display_invalidate_count++;
	zzapp_test_display_invalidate_last_address = address;
	zzapp_test_display_invalidate_last_bytes = bytes;
#else
	(void)address;
	(void)bytes;
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

/* A LATEST submission hands the frame to the card without a client request.
 * A tracked submission completes when latched or replaced.
 *
 * `back` is a buffer the app owns: the record's back buffer, or the spare
 * (the two exchange roles first, so the spare is submitted and the old back
 * becomes the spare).  Anything else is not the app's to present.  The
 * mailbox holds shown, pending and the app's buffers in three slots:
 * a pending frame is replaced and its buffer becomes the new back, otherwise
 * the spare becomes the new back. */
static int display_mailbox_flip(uint32_t back, uint64_t frame_id)
{
	struct zzapp_service_runtime *state = current_runtime();
	uint32_t spare;
	uint32_t gen;

	if (back == 0U)
		return AAR_E_INVAL;
	display_mailbox_lock_acquire();
	if (!state->display_mailbox_active) {
		display_mailbox_lock_release();
		return AAR_E_BUSY;
	}
	if (back != state->display_back) {
		if (back != state->display_spare) {
			display_mailbox_lock_release();
			return AAR_E_INVAL;
		}
		state->display_spare = state->display_back;
		state->display_back = back;
	}
	if (state->display_mailbox_pending != 0U) {
		state->display_back = state->display_mailbox_pending;
		state->display_mailbox_pending = back;
		state->display_mailbox_replaced++;
		if (state->display_mailbox_frame) {
			slot_state_lock_acquire();
			if (present_pending_locked(state, state->display_mailbox_frame))
				aar_present_finish(&state->present_feedback,
						   state->display_mailbox_frame,
						   AAR_PRESENT_REPLACED,
						   AAR_PRESENT_REASON_NONE, now_us());
			slot_state_lock_release();
		}
	} else {
		spare = state->display_spare;
		if (spare == 0U) {
			display_mailbox_lock_release();
			return AAR_E_BUSY;
		}
		state->display_mailbox_pending = back;
		state->display_back = spare;
		state->display_spare = 0U;
	}
	state->display_mailbox_frame = frame_id;
	if (frame_id)
		state->display_mailbox_started_us = now_us();
	/* Keep generic flip clients idle without exposing a VBlank wait. */
	gen = state->display_flip_gen + 1U;
	state->display_flip_gen = gen;
	state->display_flip_ack = gen;
	state->display_ack_seen = gen;
	display_mailbox_lock_release();
	return AAR_OK;
}

int zzapp_display_mailbox_retire_ready(void)
{
	struct zzapp_service_runtime *state = current_runtime();

	return !state || state->display_mailbox_gl == 0;
}

static int display_mailbox_release_dma(struct zzapp_service_runtime *state)
{
	if (!state->display_mailbox_gl)
		return AAR_OK;
	/* Only release after context close or after aborting on a halted
	 * core. An accepted DDR list can outlive the FIFO disable. */
#ifdef ZZAPP_HOST_TEST
	if (zzapp_test_display_dma_busy)
		return AAR_E_BUSY;
#else
	uint64_t began = now_us();

	while (!zzm_minigl_rasterix_idle()) {
		if (now_us() - began < 100000U)
			continue;
		printf("[app] RasterIX not idle; retaining direct display buffers\r\n");
		return AAR_E_BUSY;
	}
#endif
	slot_state_lock_acquire();
	state->display_mailbox_gl = 0;
	slot_state_lock_release();
	return AAR_OK;
}

int zzapp_display_mailbox_lease(int acquire)
{
	struct zzapp_service_runtime *state = current_runtime();
	uint32_t i;

	if (!state->display_bound || !display_mailbox_enabled())
		return AAR_E_INVAL;
	if (!acquire)
		return display_mailbox_release_dma(state);
	slot_state_lock_acquire();
	for (i = 0U; i < set_display.count; ++i) {
		zzapp_rec_t *rec = zzapp_rec_by_pid(set_display.pids[i]);

		if (rec && runtime_of(rec)->display_mailbox_gl) {
			slot_state_lock_release();
			return AAR_E_BUSY;
		}
	}
	state->display_mailbox_gl = 1;
	slot_state_lock_release();
#ifndef ZZAPP_HOST_TEST
	/* One-time CPU -> HP handoff. Clean older CPU writes out of L1
	 * and L2 before RasterIX can overwrite any of the three pages. */
	for (i = 0U; i < 3U; ++i)
		l2_maint_dcache_clean_range(state->display_mailbox_buffers[i],
					    state->display_mailbox_bytes);
#endif
	return AAR_OK;
}

/*
 * Zero-copy presentation for a module that renders straight into the
 * mailbox display's buffers (docs/app-runtime.md, "Direct presentation on
 * the mailbox display").  The three buffers are the display's own: front,
 * back and spare as the client opened them, in that order.
 */
int zzapp_display_mailbox_buffers(uint32_t buffers[3], uint32_t *bytes)
{
	struct zzapp_service_runtime *state = current_runtime();
	uint32_t i;

	if (!buffers || !state->display_bound || !display_mailbox_enabled())
		return AAR_E_INVAL;
	display_mailbox_lock_acquire();
	if (!state->display_mailbox_active) {
		display_mailbox_lock_release();
		return AAR_E_INVAL;
	}
	for (i = 0U; i < 3U; i++)
		buffers[i] = state->display_mailbox_buffers[i];
	if (bytes)
		*bytes = state->display_mailbox_bytes;
	display_mailbox_lock_release();
	return AAR_OK;
}

/* The buffers the app owns right now (0 for none), read under the mailbox
 * lock: the vblank ISR hands the old shown buffer back as the spare while
 * it holds the same lock.  The shown buffer and the pending one are never
 * in this pair. */
int zzapp_display_mailbox_owned(uint32_t *back, uint32_t *spare)
{
	struct zzapp_service_runtime *state = current_runtime();

	if (!back || !spare || !state->display_bound || !display_mailbox_enabled())
		return AAR_E_INVAL;
	display_mailbox_lock_acquire();
	if (!state->display_mailbox_active) {
		display_mailbox_lock_release();
		return AAR_E_INVAL;
	}
	*back = state->display_back;
	*spare = state->display_spare;
	display_mailbox_lock_release();
	return AAR_OK;
}

/* Presents `address`, which must be the app's back or spare buffer.  With
 * ZZAPP_DISPLAY_PRESENT_NO_DIRTY the caller guarantees that no CPU cache
 * holds a dirty line of the buffer (RasterIX wrote it and the CPU only
 * read it, after an invalidate): no clean is done.  Without it the frame is
 * cleaned through L1 and L2 as zzapp_svc_display_flip_async does. */
int zzapp_display_mailbox_present(uint32_t address, uint32_t flags, uint64_t frame_id)
{
	struct zzapp_service_runtime *state = current_runtime();
	uint32_t back;
	uint32_t spare;

	if ((flags & ~ZZAPP_DISPLAY_PRESENT_NO_DIRTY) != 0U || address == 0U ||
	    !state->display_bound)
		return AAR_E_INVAL;
	if (!display_mailbox_enabled())
		return AAR_E_BUSY;
	if ((flags & ZZAPP_DISPLAY_PRESENT_NO_DIRTY) == 0U) {
		if (zzapp_display_mailbox_owned(&back, &spare) != AAR_OK)
			return AAR_E_BUSY;
		if (address != back && address != spare)
			return AAR_E_INVAL;
		/* The ISR can only add to the owned set during the clean. */
		flush_presented(address, state->display_bytes);
	} else {
		barrier();
	}
	return display_mailbox_flip(address, frame_id);
}

/* Invalidates L1 and L2 for a range inside one of the display's buffers, so
 * the CPU reads what RasterIX wrote through its HP port.  A range anywhere
 * else is refused: the module's own invalidate is limited to memory it owns,
 * which the display's buffers are not. */
int zzapp_display_invalidate(uint32_t address, uint32_t bytes)
{
	struct zzapp_service_runtime *state = current_runtime();
	uint32_t i;

	if (!state->display_bound || bytes == 0U)
		return AAR_E_INVAL;
	for (i = 0U; i < 3U; i++) {
		uint32_t base = state->display_alias_phys[i];
		uint32_t offset;

		if (base == 0U || address < base)
			continue;
		offset = address - base;
		if (offset > state->display_bytes ||
		    bytes > state->display_bytes - offset)
			continue;
		record_display_invalidate(address, bytes);
#ifndef ZZAPP_HOST_TEST
		l2_maint_dcache_invalidate_range((intptr_t)address, bytes);
#endif
		return AAR_OK;
	}
	return AAR_E_INVAL;
}

/*
 * Palette updates do not block (aar_display_set_palette). A call writes the
 * colours into the shadow of the service block and marks the entries dirty;
 * at most one AAR_OP_DISPLAY_PALETTE request is outstanding, and it names the
 * stage, a copy that is written only while no request is outstanding. The
 * next run of dirty entries goes out when the client has answered, so updates
 * made meanwhile collapse to the latest colours. Runs from the setter and
 * from the pass for a record with a request in flight or entries waiting.
 */
static int palette_pending(zzapp_rec_t *rec)
{
	const struct zzapp_service_runtime *state = runtime_of(rec);
	unsigned i;

	if (zzapp_xport_req_state(rec, ZZAPP_XPORT_REQ_PALETTE) !=
	    ZZAPP_XPORT_REQ_FREE)
		return 1;
	for (i = 0U; i < ZZAPP_XPORT_PALETTE_ENTRIES / 32U; i++)
		if (state->palette_dirty[i] != 0U)
			return 1;
	return 0;
}

static void palette_service(zzapp_rec_t *rec)
{
	struct zzapp_service_runtime *state = runtime_of(rec);
	zzapp_xport_buf_t area;
	aar_request req;
	uint32_t first;
	uint32_t count;
	uint32_t id;

	if (!zzapp_xport_live(rec))
		return;
	if (zzapp_xport_req_state(rec, ZZAPP_XPORT_REQ_PALETTE) ==
	    ZZAPP_XPORT_REQ_POSTED)
		(void)zzapp_xport_pump(rec);
	if (zzapp_xport_req_state(rec, ZZAPP_XPORT_REQ_PALETTE) ==
	    ZZAPP_XPORT_REQ_DONE)
		/* Whatever the client answered, a colour update is not worth
		 * a stuck app. */
		(void)zzapp_xport_take_completion(rec, ZZAPP_XPORT_REQ_PALETTE,
						  NULL);
	if (zzapp_xport_req_state(rec, ZZAPP_XPORT_REQ_PALETTE) !=
		    ZZAPP_XPORT_REQ_FREE ||
	    !state->display_bound)
		return;
	/* The lowest run of dirty entries. */
	for (first = 0U; first < ZZAPP_XPORT_PALETTE_ENTRIES; first++)
		if ((state->palette_dirty[first / 32U] >> (first % 32U)) & 1U)
			break;
	if (first == ZZAPP_XPORT_PALETTE_ENTRIES)
		return;
	for (count = 1U; first + count < ZZAPP_XPORT_PALETTE_ENTRIES; count++) {
		uint32_t n = first + count;

		if (((state->palette_dirty[n / 32U] >> (n % 32U)) & 1U) == 0U)
			break;
	}
	if (zzapp_xport_service_area(rec, ZZAPP_XPORT_AREA_PALETTE, &area) !=
	    AAR_OK)
		return;
	memcpy((uint8_t *)area.ptr + ZZAPP_XPORT_PALETTE_STAGE,
	       (uint8_t *)area.ptr + first * sizeof(uint32_t),
	       count * sizeof(uint32_t));
	memset(&req, 0, sizeof(req));
	req.op = AAR_OP_DISPLAY_PALETTE;
	req.arg[AAR_A_DISPLAY_ID] = state->display_client_id;
	req.arg[AAR_A_DISPLAY_PALETTE_FIRST] = first;
	req.arg[AAR_A_DISPLAY_PALETTE_COUNT] = count;
	req.buf = area.token;
	req.buf_off = area.off + ZZAPP_XPORT_PALETTE_STAGE;
	req.buf_len = count * sizeof(uint32_t);
	/* A full SQ leaves the entries dirty for the next pass. */
	if (zzapp_xport_post_request(rec, ZZAPP_XPORT_REQ_PALETTE, &req, 0U,
				     &id) == AAR_OK) {
		uint32_t n;

		for (n = first; n < first + count; n++)
			state->palette_dirty[n / 32U] &= ~(1U << (n % 32U));
	}
}

int zzapp_svc_display_set_palette(uint32_t surface_id, uint32_t first,
				  uint32_t count, const uint32_t *rgb)
{
	struct zzapp_service_runtime *state = current_runtime();
	zzapp_rec_t *rec = service_rec();
	zzapp_xport_buf_t area;
	uint32_t i;
	int rc;

	if (!state->display_bound || surface_id != state->display_id ||
	    state->display_format != AAR_FMT_CLUT8)
		return AAR_E_INVAL;
	if (!rgb || count == 0U || first >= 256U || count > 256U - first)
		return AAR_E_INVAL;
	rc = zzapp_xport_service_area(rec, ZZAPP_XPORT_AREA_PALETTE, &area);
	if (rc != AAR_OK)
		return rc;
	for (i = 0U; i < count; i++) {
		uint32_t n = first + i;

		zzapp_put_be32((uint8_t *)area.ptr + n * sizeof(uint32_t),
			       rgb[i] & 0x00ffffffU);
		state->palette_dirty[n / 32U] |= 1U << (n % 32U);
		state->palette_set[n / 32U] |= 1U << (n % 32U);
	}
	slot_state_lock_acquire();
	set_add(&set_svc, service_pid());
	slot_state_lock_release();
	palette_service(rec);
	return AAR_OK;
}

struct flip_buffer {
	struct zzapp_service_runtime *state;
	uint64_t frame_id;
	uint32_t front, back, present;
	int nonblocking;
};

static uint32_t flip_arm_locked(struct zzapp_service_runtime *state,
				uint32_t address, uint64_t frame_id)
{
	uint32_t old_gen = state->display_flip_gen;
	uint32_t gen = old_gen + 1U;

	state->display_flip_address = address;
	state->display_flip_submit = 0U;
	state->display_flip_result = AAR_OK;
	state->display_flip_frame = frame_id;
	state->display_reply_expired = 0U;
	if (frame_id)
		state->display_flip_submit_us = now_us();
	__atomic_store_n(&state->display_flip_gen, gen ? gen : 1U,
			 __ATOMIC_RELEASE);
	return old_gen;
}

/* For a tracked flip, transport has secured the SQ slot before this runs.
 * In particular, a rejected copy fallback must not modify the front page. */
static int flip_prepare(void *context)
{
	const struct flip_buffer *buffer = context;
	struct zzapp_service_runtime *state = buffer->state;

	if ((buffer->frame_id || buffer->nonblocking) && zzapp_services_stop_requested())
		return AAR_E_IO;
	if (state->display_back_owned)
		memcpy((void *)(uintptr_t)buffer->front,
		       (const void *)(uintptr_t)buffer->back, state->display_bytes);
	flush_presented(buffer->present, state->display_bytes);
	if (!state->display_back_owned && state->display_spare != 0U) {
		state->display_queued = buffer->back;
		state->display_back = state->display_spare;
		state->display_spare = 0U;
	}
	if (buffer->frame_id || buffer->nonblocking) {
		slot_state_lock_acquire();
		(void)flip_arm_locked(state, buffer->present, buffer->frame_id);
		if (buffer->frame_id)
			set_add(&set_svc, service_pid());
		slot_state_lock_release();
	}
	return AAR_OK;
}

/* The flip request of the pan or buffer-change path. */
static int flip_post(zzapp_rec_t *rec, struct flip_buffer *buffer, uint32_t flags)
{
	struct zzapp_service_runtime *state = runtime_of(rec);
	aar_request req;
	uint32_t old_gen;
	int rc;

	memset(&req, 0, sizeof(req));
	req.op = AAR_OP_DISPLAY_FLIP;
	req.arg[AAR_A_DISPLAY_ID] = state->display_client_id;
	req.arg[AAR_A_DISPLAY_FLIP_FLAGS] = flags;
	/* The GL system worker must never park or modify the app's wait. */
	if (buffer->nonblocking)
		return zzapp_xport_post_request_prepared(rec, ZZAPP_XPORT_REQ_FLIP,
							 &req, 0U, NULL,
							 flip_prepare, buffer);
	if (buffer->frame_id)
		return req_post_prepared(ZZAPP_XPORT_REQ_FLIP, &req, 0U, NULL,
					 flip_prepare, buffer);

	(void)flip_prepare(buffer);
	/* The generation moves before the post: a completion is applied against it. */
	slot_state_lock_acquire();
	old_gen = flip_arm_locked(state, buffer->present, 0U);
	slot_state_lock_release();
	rc = req_post(ZZAPP_XPORT_REQ_FLIP, &req, 0U, NULL);
	if (rc != AAR_OK) {
		slot_state_lock_acquire();
		__atomic_store_n(&state->display_flip_gen, old_gen, __ATOMIC_RELEASE);
		slot_state_lock_release();
		return rc;
	}
	return AAR_OK;
}

static int display_flip(uint32_t surface_id, uint32_t flags, uint64_t frame_id,
			int nonblocking)
{
	struct zzapp_service_runtime *state = current_runtime();
	struct flip_buffer buffer;

	if (!state || !state->display_bound || surface_id != state->display_id)
		return AAR_E_INVAL;
	if (!zzapp_display_flip_idle())
		return AAR_E_BUSY;
	buffer.state = state;
	buffer.frame_id = frame_id;
	buffer.nonblocking = nonblocking;
	buffer.front = state->display_front;
	buffer.back = state->display_back;
	if (display_mailbox_enabled()) {
		if (state->display_mailbox_gl)
			return AAR_E_BUSY;
		if ((frame_id || nonblocking) && zzapp_services_stop_requested())
			return AAR_E_IO;
		return zzapp_display_mailbox_present(buffer.back, 0U, frame_id);
	}
	if (state->display_back_owned) {
		if (buffer.front == 0U || buffer.back == 0U)
			return AAR_E_INVAL;
		buffer.present = buffer.front;
	} else {
		buffer.present = buffer.back != 0U ? buffer.back : buffer.front;
	}
	return flip_post(service_rec(), &buffer, flags);
}

int zzapp_svc_display_flip_async(uint32_t surface_id)
{
	return display_flip(surface_id, AAR_FLIPF_ASYNC, 0U, 0);
}

int zzapp_svc_display_flip(uint32_t surface_id)
{
	int rc = display_flip(surface_id, 0U, 0U, 0);

	if (rc != AAR_OK)
		return rc;
	return wait_flip(current_runtime()->display_flip_gen, 0);
}

int zzapp_display_flip_tracked(uint32_t surface_id, uint64_t frame_id)
{
	if (frame_id) {
		struct zzapp_service_runtime *state = current_runtime();
		int pending;

		slot_state_lock_acquire();
		pending = state && present_pending_locked(state, frame_id);
		slot_state_lock_release();
		if (!pending)
			return AAR_E_INVAL;
	}
	return display_flip(surface_id, AAR_FLIPF_ASYNC, frame_id,
			    zzapp_kernel_current_thread() < 0);
}

void zzapp_display_finish_tracked(uint64_t frame_id, uint32_t reason)
{
	struct zzapp_service_runtime *state = current_runtime();

	if (!state || !frame_id)
		return;
	slot_state_lock_acquire();
	if (present_pending_locked(state, frame_id))
		aar_present_finish(&state->present_feedback, frame_id,
				   AAR_PRESENT_UNCONFIRMED, reason, now_us());
	slot_state_lock_release();
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
	yield_once();
	return AAR_OK;
}

/* The record's display back to unbound: off the vblank set, the words the
 * ISR reads cleared. The grants and the kernel lease are the caller's. */
static void display_unbind(zzapp_rec_t *rec)
{
	struct zzapp_service_runtime *state = runtime_of(rec);

	display_mailbox_disable();
	slot_state_lock_acquire();
	/* Preserve a client acknowledgement already harvested during close. */
	if ((state->display_flip_frame || state->display_reply_expired) &&
	    zzapp_xport_req_state(rec, ZZAPP_XPORT_REQ_FLIP) == ZZAPP_XPORT_REQ_DONE)
		flip_apply_locked(rec, 0);
	display_cancel_pending_locked(rec);
	state->display_bound = 0;
	state->display_id = 0U;
	state->display_ack_seen = 0U;
	state->display_queued = 0U;
	state->display_flip_gen = 0U;
	state->display_flip_ack = 0U;
	state->display_flip_submit = 0U;
	state->display_reply_expired = 0U;
	state->display_wait_feedback = 0;
	memset(state->palette_dirty, 0, sizeof(state->palette_dirty));
	state->display_flags = 0U;
	state->vblank_waiting = 0U;
	state->display_caps = 0U;
	memset(state->palette_set, 0, sizeof(state->palette_set));
	state->display_flip_address = 0U;
	state->display_source = 0U;
	set_del(&set_display, rec->hdr.pid);
	slot_state_lock_release();
}

int zzapp_svc_display_close(uint32_t surface_id)
{
	struct zzapp_service_runtime *state = current_runtime();
	uint32_t id;

	if (!state->display_bound || surface_id != state->display_id)
		return AAR_E_INVAL;
	if (state->display_mailbox_gl)
		return AAR_E_BUSY;
	/* Disable the mailbox before the client can free a pending page. */
	display_mailbox_disable();
	id = state->display_client_id;
	/* The client closes its screen; the surface goes whatever it answers. */
	(void)blocking_request(AAR_OP_DISPLAY_CLOSE, ZZAPP_WAIT_DISPLAY_OPEN, 0U,
			       &id, 1U, NULL, 0U, NULL);
	display_unbind(service_rec());
	zzapp_xport_abandon(service_rec(), ZZAPP_XPORT_REQ_FLIP);
	revoke_surface_grants();
	zzapp_kernel_surface_unbind(zzapp_runtime_kernel(), service_pid());
	drop_owned_back();
	return AAR_OK;
}

/* ---- Display, minor 6 --------------------------------------------------------- */

static int display_named(const struct zzapp_service_runtime *state,
			 uint32_t surface_id)
{
	return state && state->display_bound && surface_id == state->display_id;
}

int zzapp_svc_display_info(uint32_t surface_id, aar_display_info *out)
{
	struct zzapp_service_runtime *state = current_runtime();
	aar_display_info info;
	aar_completion c;
	uint32_t arg;
	uint32_t size;
	int rc;

	if (!out || !display_named(state, surface_id))
		return AAR_E_INVAL;
	size = out->size;
	arg = state->display_client_id;
	rc = blocking_request(AAR_OP_DISPLAY_INFO, ZZAPP_WAIT_DISPLAY_OPEN, 0U,
			      &arg, 1U, NULL, 0U, &c);
	if (rc != AAR_OK)
		return rc;
	memset(&info, 0, sizeof(info));
	info.size = size < sizeof(info) ? size : (uint32_t)sizeof(info);
	/* The surface size is the runtime's own; what the client knows is the
	 * screen. WINDOW and SCALED are what was granted. */
	info.width = state->display_width;
	info.height = state->display_height;
	info.refresh_mhz = c.out[AAR_R_DISPLAY_INFO_REFRESH_MHZ];
	info.screen_width = c.out[AAR_R_DISPLAY_INFO_SCREEN_W];
	info.screen_height = c.out[AAR_R_DISPLAY_INFO_SCREEN_H];
	info.flags = c.out[AAR_R_DISPLAY_INFO_FLAGS] & AAR_DISPINFO_ACTIVE;
	if ((state->display_caps & AAR_DISPCAP_WINDOW) != 0U)
		info.flags |= AAR_DISPINFO_WINDOW;
	if ((state->display_caps & AAR_DISPCAP_SCALED) != 0U)
		info.flags |= AAR_DISPINFO_SCALED;
	memcpy(out, &info, size < sizeof(info) ? size : sizeof(info));
	return AAR_OK;
}

/* No display is needed. The records come back in a staging buffer as six
 * big-endian words each; *out_n is the number of modes the client has in
 * all, and AAR_E_NOSPC says there are more than were written. The first try
 * stages at most the scratch area; when the client has more modes than that
 * and the caller has room for them, the request is made again with a buffer
 * of the data block's size at most (ZZAPP_XPORT_DATA_MAX bytes of records,
 * the transport's capacity, which has no offset to continue with). */
int zzapp_svc_display_modes(aar_display_mode *out, uint32_t max,
			    uint32_t *out_n)
{
	const uint32_t cap = ZZAPP_XPORT_DATA_MAX / AAR_DISPLAY_MODE_REC_BYTES;
	zzapp_rec_t *rec = service_rec();
	zzapp_xport_buf_t area = { NULL, 0U, 0U, 0U };
	uint32_t want;
	uint32_t n;
	uint32_t total;
	uint32_t i;
	int again = 1;

	if (!out_n)
		return AAR_E_INVAL;
	*out_n = 0U;
	if (max != 0U && !out)
		return AAR_E_INVAL;
	if (!rec)
		return AAR_E_IO;
	want = max < ZZAPP_XPORT_SCRATCH_BYTES / AAR_DISPLAY_MODE_REC_BYTES ?
		max : ZZAPP_XPORT_SCRATCH_BYTES / AAR_DISPLAY_MODE_REC_BYTES;
	for (;;) {
		aar_completion c;
		int rc;

		if (want != 0U) {
			rc = stage_buffer(rec, want * AAR_DISPLAY_MODE_REC_BYTES,
					  &area);
			if (rc != AAR_OK)
				return rc;
		}
		rc = blocking_request(AAR_OP_DISPLAY_MODES,
				      ZZAPP_WAIT_DISPLAY_OPEN, 0U, NULL, 0U,
				      want != 0U ? &area : NULL,
				      want * AAR_DISPLAY_MODE_REC_BYTES, &c);
		if (rc != AAR_OK)
			return rc;
		n = c.out[AAR_R_DISPLAY_MODES_COUNT];
		total = c.out[AAR_R_DISPLAY_MODES_TOTAL];
		if (n > want)
			return AAR_E_IO;
		if (total < n)
			total = n;
		if (again && total > want && max > want) {
			uint32_t more = total < max ? total : max;

			if (more > cap)
				more = cap;
			if (more > want) {
				want = more;
				again = 0;
				continue;
			}
		}
		break;
	}
	for (i = 0U; i < n; i++) {
		const uint8_t *r = (const uint8_t *)area.ptr +
				   i * AAR_DISPLAY_MODE_REC_BYTES;

		out[i].width = zzapp_get_be32(r);
		out[i].height = zzapp_get_be32(r + 4U);
		out[i].refresh_mhz = zzapp_get_be32(r + 8U);
		out[i].formats = zzapp_get_be32(r + 12U) &
				 ((1U << AAR_FMT_RGB565) |
				  (1U << AAR_FMT_BGRA8888) |
				  (1U << AAR_FMT_CLUT8));
		out[i].flags = zzapp_get_be32(r + 16U) & AAR_MODEF_PREFERRED;
		out[i].reserved = 0U;
	}
	*out_n = total;
	return total > n ? AAR_E_NOSPC : AAR_OK;
}

/*
 * The client reallocates the bitmap, so the moment its reply is in the old
 * surface is gone: the old mappings, the kernel's lease and a runtime-owned
 * back buffer are returned first, then the new surface is validated and
 * mapped by the same code as an open. A reply that cannot be used leaves no
 * display at all (the client is told to close it) rather than mappings of
 * memory the client has freed. A refusal before the reply (no grant, a bad
 * size, a flip pending, an error from the client) changes nothing.
 */
int zzapp_svc_display_resize(uint32_t surface_id, uint32_t w, uint32_t h,
			     aar_surface *out)
{
	struct zzapp_service_runtime *state = current_runtime();
	zzapp_rec_t *rec = service_rec();
	aar_completion c;
	uint32_t args[3];
	uint32_t i;
	int rc;

	if (!out || !display_named(state, surface_id))
		return AAR_E_INVAL;
	if ((state->display_caps & AAR_DISPCAP_RESIZE) == 0U)
		return AAR_E_NOSYS;
	if (w == 0U || h == 0U || w > AAR_DISPLAY_MAX_DIM ||
	    h > AAR_DISPLAY_MAX_DIM)
		return AAR_E_INVAL;
	if (!zzapp_display_flip_idle())
		return AAR_E_BUSY;
	args[AAR_A_DISPLAY_ID] = state->display_client_id;
	args[AAR_A_DISPLAY_RESIZE_W] = w;
	args[AAR_A_DISPLAY_RESIZE_H] = h;
	rc = blocking_request(AAR_OP_DISPLAY_RESIZE, ZZAPP_WAIT_DISPLAY_OPEN,
			      0U, args, 3U, NULL, 0U, &c);
	if (rc != AAR_OK)
		return rc;
	revoke_surface_grants();
	zzapp_kernel_surface_unbind(zzapp_runtime_kernel(), service_pid());
	drop_owned_back();
	state->display_req_w = w;
	state->display_req_h = h;
	rc = display_bind(rec, state->display_flags, &c, 1);
	if (rc != AAR_OK) {
		uint32_t id = c.out[AAR_R_DISPLAY_OPEN_ID];

		display_unbind(rec);
		(void)blocking_request(AAR_OP_DISPLAY_CLOSE,
				       ZZAPP_WAIT_DISPLAY_OPEN, 0U, &id, 1U,
				       NULL, 0U, NULL);
		return rc;
	}
	/* A replaced screen starts with its own colours: send the app's again. */
	if (state->display_format == AAR_FMT_CLUT8) {
		for (i = 0U; i < ZZAPP_XPORT_PALETTE_ENTRIES / 32U; i++)
			state->palette_dirty[i] |= state->palette_set[i];
		palette_service(rec);
	}
	return fill_surface(out);
}

int zzapp_svc_display_set_title(uint32_t surface_id, const char *title)
{
	struct zzapp_service_runtime *state = current_runtime();
	zzapp_xport_buf_t buf;
	uint32_t arg;
	size_t n;
	int rc;

	if (!title || !display_named(state, surface_id))
		return AAR_E_INVAL;
	/* At most 255 bytes of UTF-8, then the NUL. */
	for (n = 0U; n <= ZZAPP_SVC_PATH_MAX && title[n] != '\0'; n++)
		;
	if (n > ZZAPP_SVC_PATH_MAX)
		return AAR_E_INVAL;
	rc = zzapp_xport_service_area(service_rec(), ZZAPP_XPORT_AREA_SCRATCH,
				      &buf);
	if (rc != AAR_OK)
		return rc;
	memcpy(buf.ptr, title, n + 1U);
	arg = state->display_client_id;
	return blocking_request(AAR_OP_DISPLAY_TITLE, ZZAPP_WAIT_DISPLAY_OPEN,
				0U, &arg, 1U, &buf, (uint32_t)(n + 1U), NULL);
}

int zzapp_svc_display_pointer(uint32_t surface_id, uint32_t mode,
			      const aar_pointer *image)
{
	struct zzapp_service_runtime *state = current_runtime();
	zzapp_xport_buf_t buf;
	uint32_t args[6];
	uint32_t bytes = 0U;
	uint32_t i;
	int rc;

	if (!display_named(state, surface_id) || mode > AAR_POINTER_CUSTOM)
		return AAR_E_INVAL;
	memset(args, 0, sizeof(args));
	args[AAR_A_DISPLAY_ID] = state->display_client_id;
	args[AAR_A_DISPLAY_POINTER_MODE] = mode;
	if (mode != AAR_POINTER_CUSTOM)
		return blocking_request(AAR_OP_DISPLAY_POINTER,
				       ZZAPP_WAIT_DISPLAY_OPEN, 0U, args, 6U,
				       NULL, 0U, NULL);
	/* An image the pointer reads once: its pixels go out as big-endian
	 * words and the runtime keeps nothing of the caller's. */
	if (!image || !image->argb || image->width == 0U ||
	    image->width > AAR_POINTER_MAX_DIM || image->height == 0U ||
	    image->height > AAR_POINTER_MAX_DIM || image->hot_x < 0 ||
	    image->hot_y < 0 || (uint32_t)image->hot_x >= image->width ||
	    (uint32_t)image->hot_y >= image->height)
		return AAR_E_INVAL;
	bytes = image->width * image->height * 4U;
	rc = stage_buffer(service_rec(), bytes, &buf);
	if (rc != AAR_OK)
		return rc;
	for (i = 0U; i < image->width * image->height; i++)
		zzapp_put_be32((uint8_t *)buf.ptr + i * 4U, image->argb[i]);
	args[AAR_A_DISPLAY_POINTER_W] = image->width;
	args[AAR_A_DISPLAY_POINTER_H] = image->height;
	args[AAR_A_DISPLAY_POINTER_HOT_X] = (uint32_t)image->hot_x;
	args[AAR_A_DISPLAY_POINTER_HOT_Y] = (uint32_t)image->hot_y;
	return blocking_request(AAR_OP_DISPLAY_POINTER, ZZAPP_WAIT_DISPLAY_OPEN,
				0U, args, 6U, &buf, bytes, NULL);
}

/*
 * Parks until the next vertical blank of the display's screen.
 *
 * A display the card scans out directly (no window, not scaled) is waited on
 * here: core 0's blank callbacks count a blank for the record only when the
 * address VDMA was just pointed at is one of the display's buffers (or, for a
 * card-owned LATEST display, the one the mailbox matched), so a screen the
 * app does not own is never counted. The wait is a registered parked wait the
 * system pass wakes at the count, ends in AAR_E_TIMEDOUT exactly at the
 * timeout when no blank was seen, and in AAR_E_IO on a stop; a blank that
 * lands with either still counts.
 *
 * A window or scaled display belongs to the client, whose screen the card
 * does not scan out: the request goes to the client, which completes it at
 * the blank, or with AAR_E_TIMEDOUT at the timeout, or with AAR_E_NOSYS when
 * it cannot see one, and those results go back unchanged. The runtime's own
 * deadline there (the timeout and a short grace, ZZAPP_VBLANK_GRACE_US) only
 * covers a client that answers nothing, and ends in AAR_E_TIMEDOUT too. The
 * card's own clock is never used for it.
 *
 * Exception: a client that named the P96 screen owning the window
 * (AAR_DISP_REPLY_SCANOUT, a valid address) is waited on natively like a
 * direct-scanout display, counting only the blanks that scan out exactly
 * that screen. Without it, or with an unusable name, the wait is delegated.
 */
static int display_native_vblank(struct zzapp_service_runtime *state,
				 uint32_t timeout_us)
{
	uint64_t deadline = now_us() + timeout_us;
	uint32_t start;
	int rc;

	slot_state_lock_acquire();
	start = state->vblank_seq;
	__atomic_store_n(&state->vblank_waiting, 1U, __ATOMIC_RELEASE);
	slot_state_lock_release();
	for (;;) {
		int status;

		if (__atomic_load_n(&state->vblank_seq, __ATOMIC_ACQUIRE) != start) {
			rc = AAR_OK;
			break;
		}
		if (zzapp_services_stop_requested()) {
			rc = AAR_E_IO;
			break;
		}
		if (now_us() >= deadline) {
			rc = AAR_E_TIMEDOUT;
			break;
		}
		wait_begin(ZZAPP_WAIT_FLIP, start, deadline);
		yield_wait_once();
		status = wait_status();
		if (__atomic_load_n(&state->vblank_seq, __ATOMIC_ACQUIRE) != start) {
			rc = AAR_OK;
			break;
		}
		if (status != AAR_OK) {
			rc = status;
			break;
		}
	}
	slot_state_lock_acquire();
	__atomic_store_n(&state->vblank_waiting, 0U, __ATOMIC_RELEASE);
	slot_state_lock_release();
	wait_end();
	return rc;
}

int zzapp_svc_display_wait_vblank(uint32_t timeout_us)
{
	struct zzapp_service_runtime *state = current_runtime();
	uint32_t args[2];

	if (timeout_us == 0U || !state || !state->display_bound)
		return AAR_E_INVAL;
	if (!display_client_owned(state) || state->display_source != 0U)
		return display_native_vblank(state, timeout_us);
	args[AAR_A_DISPLAY_ID] = state->display_client_id;
	args[AAR_A_DISPLAY_VBLANK_TIMEOUT_US] = timeout_us;
	return blocking_request(AAR_OP_DISPLAY_VBLANK, ZZAPP_WAIT_DISPLAY_OPEN,
				now_us() + timeout_us + ZZAPP_VBLANK_GRACE_US,
				args, 2U, NULL, 0U, NULL);
}

uint64_t zzapp_svc_display_last_present_us(void)
{
	struct zzapp_service_runtime *state = current_runtime();
	uint64_t t;

	if (!state)
		return 0U;
	slot_state_lock_acquire();
	t = state->display_present_us;
	slot_state_lock_release();
	return t;
}

int zzapp_svc_display_present(uint32_t surface_id, uint32_t flags,
			      uint64_t *frame_id)
{
	zzapp_rec_t *rec = service_rec();
	struct zzapp_service_runtime *state = current_runtime();
	uint64_t id;
	int rc;

	if (!frame_id)
		return AAR_E_INVAL;
	*frame_id = 0U;
	if (!display_named(state, surface_id) ||
	    (flags != 0U && flags != AAR_PRESENT_ASYNC && flags != AAR_PRESENT_GL))
		return AAR_E_INVAL;
	if (stop_requested_rec(rec))
		return AAR_E_IO;
	slot_state_lock_acquire();
	id = aar_present_reserve(&state->present_feedback);
	if (id)
		state->present_submitting = 1;
	slot_state_lock_release();
	if (!id)
		return AAR_E_BUSY;

	rc = flags == AAR_PRESENT_GL ? zzapp_minigl_swap_tracked(surface_id, id) :
	     display_flip(surface_id, flags == AAR_PRESENT_ASYNC ? AAR_FLIPF_ASYNC : 0U,
			  id, 0);
	slot_state_lock_acquire();
	state->present_submitting = 0;
	if (rc != AAR_OK)
		aar_present_discard(&state->present_feedback, id);
	display_cancel_on_stop_locked(rec);
	if (state->present_feedback.pending)
		set_add(&set_svc, rec->hdr.pid);
	slot_state_lock_release();
	if (rc != AAR_OK)
		return rc;

	*frame_id = id;
	if (flags == 0U && wait_flip(state->display_flip_gen, 1) != AAR_OK) {
		slot_state_lock_acquire();
		display_cancel_on_stop_locked(rec);
		slot_state_lock_release();
	}
	return AAR_OK;
}

int zzapp_svc_display_present_poll(aar_present_result *out)
{
	zzapp_rec_t *rec = service_rec();
	int rc;

	if (!out || !rec)
		return AAR_E_INVAL;
	slot_state_lock_acquire();
	display_cancel_on_stop_locked(rec);
	rc = aar_present_poll(&runtime_of(rec)->present_feedback, out);
	slot_state_lock_release();
	return rc;
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

/* The runtime counts a played underrun in the ring header (spec 8.4): the
 * word at AAR_AUDIO_RING_UNDERRUNS, the runtime's own, beside read_frame. The
 * ring is normal write-back shareable memory that the client reads through
 * the ACP, so a plain store is enough (docs/app-runtime.md, host-visible
 * memory). The caller holds the slot-state lock; the ring is little-endian. */
_Static_assert(AAR_AUDIO_RING_UNDERRUNS == AAR_AUDIO_RING_READ_FRAME + 4U,
	       "the underrun word follows read_frame");
static void audio_note_underrun(struct zzapp_service_runtime *state)
{
	volatile uint32_t *word;

	if (!state->audio_read_frame)
		return;
	word = state->audio_read_frame + 1;
	*word = *word + 1U;
}

#ifndef ZZAPP_HOST_TEST
static uint32_t runtime_pid(const struct zzapp_service_runtime *state)
{
	return ((const zzapp_rec_t *)(const void *)((const char *)state -
		offsetof(zzapp_rec_t, services)))->hdr.pid;
}

/* The app whose ring feeds a producer slot. The caller holds
 * g_slot_state_lock and uses the result inside the same section. */
static struct zzapp_service_runtime *audio_runtime(uint32_t producer_slot)
{
	uint32_t i;

	for (i = 0U; i < set_core0.count; i++) {
		zzapp_rec_t *rec = zzapp_rec_by_pid(set_core0.pids[i]);
		struct zzapp_service_runtime *state;

		if (!rec)
			continue;
		state = runtime_of(rec);
		if (state->audio_open && !state->audio_ahi_route &&
		    state->audio_slot == producer_slot)
			return state;
	}
	return NULL;
}

static int app_audio_snapshot_for(uint32_t producer_slot,
				  struct audio_fabric_source *source)
{
	struct zzapp_service_runtime *state;
	uint32_t wr;
	uint64_t staged;
	int snapshot = 0;

	slot_state_lock_acquire();
	state = audio_runtime(producer_slot);
	if (!state || !state->audio_samples || !state->audio_write_frame)
		goto out;
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
	/* The app stores PCM through its cacheable, shareable alias of the
	 * ring: the fabric reads it without invalidating. */
	source->coherent = 1U;
#if ZZAPP_PERF_COUNTERS
	if (source->produced_bytes - source->staged_bytes <= source->capacity / 4U) {
		if (!state->audio_low_water_latched) {
			zzapp_kernel_perf_audio(zzapp_runtime_kernel(),
						runtime_pid(state), 1U, 0U,
						0U);
			state->audio_low_water_latched = 1U;
		}
	} else {
		state->audio_low_water_latched = 0U;
	}
#endif
	snapshot = 1;
out:
	slot_state_lock_release();
	return snapshot;
}

static int app_audio_stage_for(uint32_t producer_slot, uint32_t bytes)
{
	struct zzapp_service_runtime *state;
	uint64_t produced;
	int staged = 0;

	slot_state_lock_acquire();
	state = audio_runtime(producer_slot);
	if (state && state->audio_write_frame) {
		produced = audio_produced_frames(*state->audio_write_frame,
						 state->audio_staged) * 4U;
		if (produced >= state->audio_staged &&
		    produced - state->audio_staged >= bytes) {
			state->audio_staged += bytes;
			staged = 1;
		}
	}
	slot_state_lock_release();
	return staged;
}

static void app_audio_retire_for(uint32_t producer_slot, uint32_t bytes)
{
	struct zzapp_service_runtime *state;

	slot_state_lock_acquire();
	state = audio_runtime(producer_slot);
	if (state && state->audio_read_frame && (bytes & 3U) == 0U)
		*state->audio_read_frame += bytes / 4U;
	slot_state_lock_release();
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
	struct zzapp_service_runtime *state;

	slot_state_lock_acquire();
	state = audio_runtime(producer_slot);
	if (state)
		zzapp_kernel_perf_audio(zzapp_runtime_kernel(),
					runtime_pid(state), 0U, 1U, 0U);
	slot_state_lock_release();
}
#endif

static void app_audio_underrun_for(uint32_t producer_slot)
{
	struct zzapp_service_runtime *state;

	slot_state_lock_acquire();
	state = audio_runtime(producer_slot);
	if (state) {
		audio_note_underrun(state);
#if ZZAPP_PERF_COUNTERS
		zzapp_kernel_perf_audio(zzapp_runtime_kernel(),
					runtime_pid(state), 0U, 0U, 1U);
#endif
	}
	slot_state_lock_release();
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

static void revoke_audio_grant(void)
{
	struct zzapp_service_runtime *state = current_runtime();
	zzapp_proc_t *proc = service_proc();

	if (proc && state->audio_grant_bytes != 0U)
		(void)zzapp_as_alias_unmap(&proc->as, state->audio_grant_base,
					    state->audio_grant_bytes);
	state->audio_grant_base = 0U;
	state->audio_grant_bytes = 0U;
	state->audio_alias = 0U;
}


static void audio_detach(void)
{
	struct zzapp_service_runtime *state = current_runtime();
	zzapp_rec_t *rec = service_rec();
	int open = 1;
	int was_ahi_route;
	int keep;

	/* Core 0 releases a close only after core 1 has quiesced. */
	if (!__atomic_compare_exchange_n(&state->audio_open, &open, 0, 0,
					 __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
		return;
#ifdef ZZAPP_HOST_TEST
	state->audio_detach_count++;
#endif
	was_ahi_route = state->audio_ahi_route;
	revoke_audio_grant();
#ifndef ZZAPP_HOST_TEST
	if (!was_ahi_route)
		audio_fabric_producer_detach(state->audio_slot);
#else
	(void)was_ahi_route;
#endif
	/* The ring goes back with its block pointer cleared first (the
	 * transport does that), so a replayed teardown frees nothing twice.
	 * The client of the AHI route maps the ring and writes READ_FRAME
	 * through it, so the ring stays, and the shell's release returns it,
	 * whenever the client has not been told to stop and answered: a
	 * quarantined claim (the client may still write until it finds out), an
	 * app that ended without closing the stream, and, through the request
	 * it still names (zzapp_xport_buffer_held in block_free), an AUDIO_OPEN
	 * or AUDIO_CLOSE the client has not completed. */
	state->audio_samples = NULL;
	state->audio_read_frame = NULL;
	state->audio_write_frame = NULL;
	keep = zzapp_xport_flag_test(rec, ZZAPP_XPORT_F_QUARANTINED) ||
	       (was_ahi_route && state->audio_teardown);
	if (!keep)
		(void)zzapp_xport_block_free(rec, ZZAPP_XPORT_BLOCK_AUDIO);
	state->audio_staged = 0U;
	state->audio_first_write_seen = 0U;
	state->audio_ahi_route = 0;
#if ZZAPP_PERF_COUNTERS
	state->audio_low_water_latched = 0U;
#endif
	zzapp_kernel_audio_close(zzapp_runtime_kernel(), service_pid());
	service_proc()->audio_direct = 0U;
}

/* Core 0 (or the host test inline): the ring block, its grant into the app,
 * the header the client reads, and the card route's fabric attach. The AHI
 * route's open request to the client is the app thread's, afterwards. */
static int audio_attach(void)
{
	struct zzapp_service_runtime *state = current_runtime();
	zzapp_rec_t *rec = service_rec();
	zzapp_xport_buf_t ring;
	uint32_t slot = 0U;
	uint32_t bytes = AAR_AUDIO_RING_HDR_BYTES + ZZAPP_AUDIO_FRAMES * 4U;
	uint8_t *base;
	int rc;

	state->audio_ahi_route =
		(rec->claim.client_caps & ZZAPP_CLIENT_AUDIO_AHI) != 0U;
	if (state->audio_open || zzapp_xport_block_get(rec, ZZAPP_XPORT_BLOCK_AUDIO,
						       NULL))
		return AAR_E_BUSY;
	if (!state->audio_ahi_route) {
		slot = AUDIO_FABRIC_SLOT_RESERVED;
		if (slot_is_busy(slot))
			slot = AUDIO_FABRIC_SLOT_MAILBOX;
		if (slot_is_busy(slot))
			return AAR_E_BUSY;
	}
#ifndef ZZAPP_HOST_TEST
	if (!state->audio_ahi_route &&
	    !audio_fabric_conversion_admissible(ZZAPP_AUDIO_RATE))
		return AAR_E_BUSY;
#endif
	rc = zzapp_xport_block_alloc(rec, ZZAPP_XPORT_BLOCK_AUDIO,
				     ZZAPP_XPORT_AUDIO_BYTES(ZZAPP_AUDIO_FRAMES),
				     &ring);
	if (rc != AAR_OK)
		return AAR_E_NOMEM;
	if (grant_user_buffer(ring.token + zzapp_xport_env()->token_adjust,
			      bytes, &state->audio_grant_base,
			      &state->audio_grant_bytes,
			      &state->audio_alias) != AAR_OK) {
		(void)zzapp_xport_block_free(rec, ZZAPP_XPORT_BLOCK_AUDIO);
		return AAR_E_NOMEM;
	}
	base = (uint8_t *)ring.ptr;
	memset(base, 0, AAR_AUDIO_RING_HDR_BYTES);
	/* The ring is little-endian (transport.h): plain stores on both ARM and
	 * the host. */
	*(volatile uint32_t *)(base + AAR_AUDIO_RING_RATE) = ZZAPP_AUDIO_RATE;
	*(volatile uint32_t *)(base + AAR_AUDIO_RING_CHANNELS) = 2U;
	*(volatile uint32_t *)(base + AAR_AUDIO_RING_FORMAT) =
		AAR_AUDIO_FORMAT_S16LE;
	state->audio_write_frame = (volatile uint32_t *)(base +
		AAR_AUDIO_RING_WRITE_FRAME);
	state->audio_read_frame = (volatile uint32_t *)(base +
		AAR_AUDIO_RING_READ_FRAME);
	state->audio_samples = (int16_t *)(base + AAR_AUDIO_RING_HDR_BYTES);
	state->audio_staged = 0U;
	state->audio_first_write_seen = 0U;
	state->audio_slot = slot;
#if ZZAPP_PERF_COUNTERS
	state->audio_low_water_latched = 0U;
#endif
	/* The ISR may snapshot as soon as go_live returns. */
	state->audio_open = 1;
	barrier();
#ifndef ZZAPP_HOST_TEST
	if (!state->audio_ahi_route) {
		if (!audio_fabric_producer_attach(slot, app_audio_ops_for_slot(slot))) {
			state->audio_open = 0;
			revoke_audio_grant();
			state->audio_samples = NULL;
			state->audio_read_frame = NULL;
			state->audio_write_frame = NULL;
			(void)zzapp_xport_block_free(rec, ZZAPP_XPORT_BLOCK_AUDIO);
			return AAR_E_BUSY;
		}
		audio_fabric_producer_rate_set(slot, ZZAPP_AUDIO_RATE);
	}
#endif
	return AAR_OK;
}

static void audio_fill(aar_audio *out)
{
	uintptr_t alias = current_runtime()->audio_alias;

	out->samples = (int16_t *)(alias + AAR_AUDIO_RING_HDR_BYTES);
	out->frames = ZZAPP_AUDIO_FRAMES;
	out->read_frame = (volatile uint32_t *)(alias + AAR_AUDIO_RING_READ_FRAME);
	out->write_frame = (volatile uint32_t *)(alias +
						 AAR_AUDIO_RING_WRITE_FRAME);
	out->rate = ZZAPP_AUDIO_RATE;
	out->channels = 2U;
	out->flags = 0U;
}

/* Raise a request to core 0 and make sure core 0's pass sees the record. */
static void audio_request_raise(struct zzapp_service_runtime *state,
				uint32_t request)
{
	slot_state_lock_acquire();
	state->audio_request = request;
	set_add(&set_core0, service_pid());
	slot_state_lock_release();
}

#ifndef ZZAPP_HOST_TEST
/* The core 0 handoff, parked: AAR_OK, or why the wait ended. */
static int audio_handoff_wait(struct zzapp_service_runtime *state)
{
	wait_begin(ZZAPP_WAIT_AUDIO, 0U, 0U);
	while (state->audio_request != AUDIO_REQUEST_NONE) {
		yield_wait_once();
		if (wait_status() != AAR_OK) {
			wait_end();
			return wait_status();
		}
	}
	wait_end();
	return AAR_OK;
}
#endif

/* Returns the ring: the AHI route's close request to the client is already
 * done; core 0 detaches (inline in the host test). */
static int audio_release_ring(struct zzapp_service_runtime *state)
{
#ifdef ZZAPP_HOST_TEST
	(void)state;
	audio_detach();
	return AAR_OK;
#else
	int rc;

	state->audio_result = AAR_E_IO;
	barrier();
	audio_request_raise(state, AUDIO_REQUEST_CLOSE);
	rc = audio_handoff_wait(state);
	if (rc != AAR_OK)
		return rc;
	return state->audio_result;
#endif
}

/* The AHI route: the client opens AHI against the ring the runtime
 * allocated and named. The grant must match the request. */
static int audio_open_client(struct zzapp_service_runtime *state,
			     uint32_t rate, uint32_t channels)
{
	zzapp_rec_t *rec = service_rec();
	zzapp_xport_buf_t ring;
	aar_completion c;
	uint32_t args[3] = { rate, channels, AAR_AUDIO_ROUTE_AHI };
	int rc;

	if (!zzapp_xport_block_get(rec, ZZAPP_XPORT_BLOCK_AUDIO, &ring))
		return AAR_E_IO;
	(void)state;
	rc = blocking_request(AAR_OP_AUDIO_OPEN, ZZAPP_WAIT_AUDIO_REQ, 0U, args, 3U,
			      &ring, AAR_AUDIO_RING_HDR_BYTES + ZZAPP_AUDIO_FRAMES * 4U,
			      &c);
	if (rc != AAR_OK)
		return rc;
	if (c.out[AAR_R_AUDIO_OPEN_RING] != ring.token + ring.off ||
	    c.out[AAR_R_AUDIO_OPEN_RATE] != rate ||
	    c.out[AAR_R_AUDIO_OPEN_CHANNELS] != channels)
		return AAR_E_IO;
	return AAR_OK;
}

int zzapp_svc_audio_open(uint32_t rate, uint32_t channels, aar_audio *out)
{
	struct zzapp_service_runtime *state = current_runtime();
	zzapp_rec_t *rec = service_rec();
	int direct_card_route;
	int rc;

	if (!out)
		return AAR_E_INVAL;
	if (channels != 2U || rate != ZZAPP_AUDIO_RATE)
		return AAR_E_INVAL;
	if (state->audio_open || state->audio_request != AUDIO_REQUEST_NONE)
		return AAR_E_BUSY;
	direct_card_route = (rec->claim.client_caps & ZZAPP_CLIENT_AUDIO_AHI) == 0U;
	if (zzapp_kernel_audio_open(zzapp_runtime_kernel(), service_pid(),
				    direct_card_route) != 0)
		return AAR_E_BUSY;
#ifdef ZZAPP_HOST_TEST
	rc = audio_attach();
#else
	state->audio_result = AAR_E_BUSY;
	barrier();
	audio_request_raise(state, AUDIO_REQUEST_OPEN);
	rc = audio_handoff_wait(state);
	if (rc == AAR_OK)
		rc = state->audio_result;
	else
		return rc;
#endif
	if (rc != AAR_OK) {
		zzapp_kernel_audio_close(zzapp_runtime_kernel(), service_pid());
		return rc;
	}
	if (!direct_card_route) {
		rc = audio_open_client(state, rate, channels);
		if (rc != AAR_OK) {
			(void)audio_release_ring(state);
			zzapp_kernel_audio_close(zzapp_runtime_kernel(),
						 service_pid());
			return rc;
		}
	}
	service_proc()->audio_direct = direct_card_route ? 1U : 0U;
	cbf_or_current(AAR_CBF_AUDIO);
	audio_fill(out);
	return AAR_OK;
}

int zzapp_svc_audio_close(void)
{
	struct zzapp_service_runtime *state = current_runtime();

	if (!state->audio_open || state->audio_request != AUDIO_REQUEST_NONE)
		return AAR_E_INVAL;
	if (state->audio_ahi_route) {
		int rc;

		/* The client stops its AHI reads when it answers the close. The
		 * ring is returned only after that answer: a close that could
		 * not be posted, whose wait failed, or that the client
		 * answered with an error leaves the stream open and the ring
		 * with the client, which may still write READ_FRAME. The app
		 * can retry the close; its end or the claim's release returns
		 * the ring. */
		rc = blocking_request(AAR_OP_AUDIO_CLOSE, ZZAPP_WAIT_AUDIO_REQ,
				      0U, NULL, 0U, NULL, 0U, NULL);
		if (rc != AAR_OK)
			return rc;
	}
	return audio_release_ring(state);
}

static void audio_post_teardown(void)
{
	struct zzapp_service_runtime *state = current_runtime();

	/* Force-stop and fault frames cannot wait for core 0. Post the same
	 * close handoff explicit close uses, and leave the slot busy until
	 * core 0 (or a halted-core reclaim) finishes the detach. */
	if (state->audio_request == AUDIO_REQUEST_CLOSE ||
	    state->audio_request == AUDIO_REQUEST_CLOSE_QUIESCED) {
		state->audio_teardown = 1;
		return;
	}
	if (!state->audio_open && !zzapp_xport_block_get(service_rec(),
		ZZAPP_XPORT_BLOCK_AUDIO, NULL) &&
	    state->audio_request != AUDIO_REQUEST_OPEN)
		return;
	state->audio_teardown = 1;
	state->audio_result = AAR_E_IO;
	barrier();
	if (state->audio_request != AUDIO_REQUEST_OPEN)
		audio_request_raise(state, AUDIO_REQUEST_CLOSE);
}

static int audio_handoff_pending(const struct zzapp_service_runtime *state)
{
	return state->audio_teardown ||
	       state->audio_request == AUDIO_REQUEST_CLOSE ||
	       state->audio_request == AUDIO_REQUEST_CLOSE_QUIESCED;
}

/* A shell held for an audio handoff is released once core 0 has finished the
 * detach: its resources were returned by retire. */
static void finish_audio_slot_release(void)
{
	zzapp_rec_t *rec = service_rec();

	if (!rec)
		return;
	slot_state_lock_acquire();
	finish_shell_release(rec);
	slot_state_lock_release();
}

static void reclaim_halted_audio_slot(void)
{
	struct zzapp_service_runtime *state = current_runtime();

	if (state->audio_request == AUDIO_REQUEST_OPEN) {
		state->audio_result = AAR_E_IO;
		state->audio_teardown = 0;
		barrier();
		state->audio_request = AUDIO_REQUEST_NONE;
		finish_audio_slot_release();
		return;
	}
	if (!state->audio_open &&
	    !zzapp_xport_block_get(service_rec(), ZZAPP_XPORT_BLOCK_AUDIO, NULL) &&
	    state->audio_request != AUDIO_REQUEST_CLOSE &&
	    state->audio_request != AUDIO_REQUEST_CLOSE_QUIESCED) {
		finish_audio_slot_release();
		return;
	}
	audio_detach();
	state->audio_teardown = 0;
	state->audio_result = AAR_OK;
	barrier();
	state->audio_request = AUDIO_REQUEST_NONE;
	finish_audio_slot_release();
}

void zzapp_services_reclaim_halted_audio(void)
{
	struct svc_ctx saved = ctx_save();
	uint32_t i;

	for (i = set_core0.count; i-- > 0U;) {
		uint32_t pid;

		if (i >= set_core0.count)
			continue;
		pid = set_core0.pids[i];
		if (!zzapp_rec_by_pid(pid) ||
		    zzapp_rec_state(pid) == ZZAPP_REC_FREE)
			continue;
		ctx_pid[service_context()] = pid;
		reclaim_halted_audio_slot();
	}
	ctx_restore(saved);
}

/*
 * The system thread's pass (core 1) over the records that have something to
 * do: a parked wait, a doorbell, console output in flight. For each: take the
 * doorbell, pump the queue, move the console stream along, then decide
 * whether a parked app can go on. Everything runs under g_slot_state_lock, so
 * core 0 never frees the blocks of a record while it is looked at; the wake
 * itself is outside it.
 */
static void console_service(zzapp_rec_t *rec);

/* A blocking request of the record has its completion. */
static int blocking_done(zzapp_rec_t *rec, uint32_t id)
{
	return zzapp_xport_req_state(rec, ZZAPP_XPORT_REQ_BLOCKING) ==
		       ZZAPP_XPORT_REQ_DONE &&
	       zzapp_xport_req_id(rec, ZZAPP_XPORT_REQ_BLOCKING) == id;
}

/* Returns the zzapp_wake_reason_t to wake with, or 0. */
static int service_one_locked(zzapp_rec_t *rec)
{
	struct zzapp_service_runtime *state = runtime_of(rec);
	int kicked;
	int ready = 0;
	int event_ready = 0;
	int pump_rc = 0;

	if (!zzapp_xport_live(rec))
		return 0;
	kicked = zzapp_xport_kick_take(rec) != 0U;
	if (kicked || state->wait_kind != ZZAPP_WAIT_NONE || console_pending(rec) ||
	    palette_pending(rec) || file_async_pending(rec) ||
	    state->display_flip_frame) {
		pump_rc = zzapp_xport_pump(rec);

		if (pump_rc < 0)
			/* The waiter finds the indices corrupt and ends. */
			ready = 1;
		file_async_check(rec, pump_rc);
	}
	console_service(rec);
	palette_service(rec);
	if (state->display_flip_frame ||
	    (kicked && state->display_reply_expired) ||
	    (state->wait_kind == ZZAPP_WAIT_FLIP && !state->vblank_waiting))
		flip_apply_locked(rec, pump_rc);
	display_cancel_on_stop_locked(rec);
	if (state->wait_kind == ZZAPP_WAIT_NONE)
		return 0;
	switch (state->wait_kind) {
	case ZZAPP_WAIT_FILE:
	case ZZAPP_WAIT_CONSOLE_READ:
	case ZZAPP_WAIT_DISPLAY_OPEN:
	case ZZAPP_WAIT_AUDIO_REQ:
		ready |= blocking_done(rec, state->wait_value);
		break;
	case ZZAPP_WAIT_SQ_SPACE:
		ready |= zzapp_xport_sq_space(rec) != 0;
		break;
	case ZZAPP_WAIT_CONSOLE_WRITE:
		ready |= state->console_tail != state->wait_value;
		break;
	case ZZAPP_WAIT_FLIP:
		/* A native vertical-blank wait is parked under this kind too,
		 * its value the blank count it started from. */
		if (state->vblank_waiting) {
			ready |= state->vblank_seq != state->wait_value;
			break;
		}
		ready |= state->display_flip_ack == state->wait_value;
		if (state->display_wait_feedback)
			ready |= flip_feedback_wait_done(state, state->wait_value);
		break;
	case ZZAPP_WAIT_AUDIO:
		/* Core 0's bounded handoff, not cancellable: the waiter does
		 * not look at a stop, so one must not wake it. */
		ready |= state->audio_request == AUDIO_REQUEST_NONE;
		break;
	case ZZAPP_WAIT_EVENT:
		/* An arrived message beats the deadline: the waiter reports a
		 * message that landed while it was timing out. */
		event_ready = zzapp_xport_msg_pending(rec) != 0U;
		break;
	default:
		break;
	}
	/* A stop ends every parked wait the spec lists as returning on stop
	 * (files, console, display, audio requests, messages; spec 3.7): the
	 * waiter looks at the stop after its completion, so a result that
	 * landed with it is still reported. */
	if (state->wait_kind != ZZAPP_WAIT_AUDIO && stop_requested_rec(rec))
		event_ready = 1;
	if (event_ready)
		return ZZAPP_WAKE_READY;
	if (state->wait_deadline != 0U && now_us() >= state->wait_deadline)
		return ZZAPP_WAKE_TIMEOUT;
	return ready ? ZZAPP_WAKE_READY : 0;
}

static int service_wanted(zzapp_rec_t *rec)
{
	return runtime_of(rec)->wait_kind != ZZAPP_WAIT_NONE ||
	       runtime_of(rec)->present_feedback.pending ||
	       console_pending(rec) || palette_pending(rec) || file_async_pending(rec);
}

static void service_pass(void)
{
	struct svc_ctx saved = ctx_save();
	uint32_t i;

	for (i = set_svc.count; i-- > 0U;) {
		zzapp_rec_t *rec;
		struct zzapp_service_runtime *state;
		uint32_t pid;
		int wake;
		int check_mailbox;

		slot_state_lock_acquire();
		if (i >= set_svc.count) {
			slot_state_lock_release();
			continue;
		}
		pid = set_svc.pids[i];
		rec = zzapp_rec_by_pid(pid);
		if (!rec || zzapp_rec_state(pid) == ZZAPP_REC_FREE) {
			set_del(&set_svc, pid);
			slot_state_lock_release();
			continue;
		}
		ctx_pid[service_context()] = pid;
		SERVICE_LOOKUP_GAP(pid);
		wake = service_one_locked(rec);
		state = runtime_of(rec);
		check_mailbox = state->present_feedback.pending &&
			__atomic_load_n(&state->display_mailbox_active, __ATOMIC_ACQUIRE);
		if (!service_wanted(rec))
			set_del(&set_svc, pid);
		/* IDLE is the launcher's permission to release the shell. A
		 * final console completion may have drained it in this pass. */
		finish_shell_release(rec);
		slot_state_lock_release();
		if (check_mailbox)
			display_mailbox_expire(state);
		if (wake != 0)
			(void)zzapp_kernel_wake(zzapp_runtime_kernel(), pid,
						(zzapp_wake_reason_t)wake);
	}
	ctx_restore(saved);
}

#define AUDIO_NO_GO_LIVE 0xffffffffU

/*
 * Core 0's audio work for one record. `request` and `go_live_slot` were read
 * under g_slot_state_lock together with the lookup. The rest runs without it
 * (an attach allocates and maps): a record with an audio request pending is
 * not freed until this handoff ends (audio_post_teardown marks the teardown,
 * audio_handoff_pending then holds the shell).
 */
static void audio_core0_poll_slot(uint32_t request, uint32_t go_live_slot)
{
	struct zzapp_service_runtime *state = current_runtime();

	if (request == AUDIO_REQUEST_OPEN) {
		if (state->audio_teardown)
			state->audio_result = AAR_E_IO;
		else
			state->audio_result = audio_attach();
		barrier();
		state->audio_request = AUDIO_REQUEST_NONE;
		if (state->audio_teardown &&
		    (state->audio_open ||
		     zzapp_xport_block_get(service_rec(), ZZAPP_XPORT_BLOCK_AUDIO,
					   NULL)))
			state->audio_request = AUDIO_REQUEST_CLOSE;
		else
			state->audio_teardown = 0;
		finish_audio_slot_release();
		return;
	}
	if (request == AUDIO_REQUEST_NONE) {
#ifndef ZZAPP_HOST_TEST
		if (go_live_slot != AUDIO_NO_GO_LIVE)
			(void)audio_fabric_producer_go_live_when_ready(
				go_live_slot, 1);
#else
		(void)go_live_slot;
#endif
		finish_audio_slot_release();
		return;
	}
	if (request != AUDIO_REQUEST_CLOSE_QUIESCED) {
		finish_audio_slot_release();
		return;
	}
	audio_detach();
	state->audio_teardown = 0;
	state->audio_result = AAR_OK;
	barrier();
	state->audio_request = AUDIO_REQUEST_NONE;
	finish_audio_slot_release();
}

/* What keeps a record in core 0's set: an audio stream or request, a deferred
 * op in flight, a claim that is loading. Caller holds g_slot_state_lock. */
static int core0_wanted(zzapp_rec_t *rec)
{
	struct zzapp_service_runtime *state = runtime_of(rec);

	if (state->audio_request != AUDIO_REQUEST_NONE || state->audio_open ||
	    state->audio_teardown || state->defer.state != DEFER_IDLE)
		return 1;
	return zzapp_rec_state(rec->hdr.pid) == ZZAPP_REC_LIVE &&
	       (rec->claim.state == ZZAPP_ST_CLAIMED ||
		rec->claim.state == ZZAPP_ST_LOADING);
}

void zzapp_services_core0_poll(void)
{
	struct svc_ctx saved = ctx_save();
	uint32_t i;

	for (i = set_core0.count; i-- > 0U;) {
		uint32_t request;
		uint32_t go_live_slot = AUDIO_NO_GO_LIVE;
		uint32_t pid;
		zzapp_rec_t *rec;

		slot_state_lock_acquire();
		if (i >= set_core0.count) {
			slot_state_lock_release();
			continue;
		}
		pid = set_core0.pids[i];
		rec = zzapp_rec_by_pid(pid);
		if (!rec || zzapp_rec_state(pid) == ZZAPP_REC_FREE) {
			set_del(&set_core0, pid);
			slot_state_lock_release();
			continue;
		}
		ctx_pid[service_context()] = pid;
		SERVICE_LOOKUP_GAP(pid);
		request = current_runtime()->audio_request;
		if (request == AUDIO_REQUEST_NONE &&
		    current_runtime()->audio_first_write_seen &&
		    current_runtime()->audio_open &&
		    !current_runtime()->audio_ahi_route)
			go_live_slot = current_runtime()->audio_slot;
		slot_state_lock_release();
		audio_core0_poll_slot(request, go_live_slot);
		slot_state_lock_acquire();
		rec = zzapp_rec_by_pid(pid);
		if (!rec || zzapp_rec_state(pid) == ZZAPP_REC_FREE ||
		    !core0_wanted(rec))
			set_del(&set_core0, pid);
		slot_state_lock_release();
	}
	ctx_restore(saved);
}

/* The name LIST shows: the last path component of argv[0], read from the
 * argv area of the load buffer. */
static void capture_start_name(zzapp_rec_t *rec, uint32_t argc)
{
	uint8_t *name = runtime_of(rec)->name;
	zzapp_xport_buf_t load;
	const uint8_t *start;
	uint32_t off;
	uint32_t end;
	uint32_t i;
	uint32_t length;

	memset(name, 0, ZZAPP_LIST_NAME_BYTES);
	if (argc == 0U || !zzapp_xport_block_get(rec, ZZAPP_XPORT_BLOCK_LOAD, &load))
		return;
	off = zzapp_get_be32((const uint8_t *)load.ptr + ZZAPP_LOAD_ARGV_OFF +
			     ZZAPP_ARGV_OFFS_OFF);
	if (off < ZZAPP_LOAD_ARGV_OFF + ZZAPP_ARGV_STR_OFF ||
	    off >= ZZAPP_LOAD_ARGV_OFF + ZZAPP_LOAD_ARGV_BYTES)
		return;
	start = (const uint8_t *)load.ptr + off;
	length = ZZAPP_LOAD_ARGV_OFF + ZZAPP_LOAD_ARGV_BYTES - off;
	if (length > ZZAPP_ARGV_STR_MAX)
		length = ZZAPP_ARGV_STR_MAX;
	end = 0U;
	for (i = 0U; i < length && start[i] != '\0'; i++) {
		if (start[i] == '/' || start[i] == ':')
			end = i + 1U;
	}
	for (i = end; i + 1U < ZZAPP_LIST_NAME_BYTES + end && i < length; i++)
		name[i - end] = start[i];
}

/* Core 1's share of the audio handoff, for every record with audio activity:
 * a close request is quiesced (the AHI route has no ring pump any more, so
 * quiescing is all there is), and a card-route ring that the app has begun
 * to fill is noted so core 0 can bring the stream live. */
static void audio_core1_poll_all(void)
{
	struct svc_ctx saved = ctx_save();
	uint32_t i;

#if ZZAPP_PERF_COUNTERS && !defined(ZZAPP_HOST_TEST)
	zzapp_kernel_perf_service_begin(zzapp_runtime_kernel(),
					ZZAPP_PERF_SERVICE_AUDIO,
					scheduler_perf_stamp());
#endif
	for (i = set_core0.count; i-- > 0U;) {
		struct zzapp_service_runtime *state;
		zzapp_rec_t *rec;

		slot_state_lock_acquire();
		if (i >= set_core0.count) {
			slot_state_lock_release();
			continue;
		}
		rec = zzapp_rec_by_pid(set_core0.pids[i]);
		if (!rec) {
			slot_state_lock_release();
			continue;
		}
		state = runtime_of(rec);
		SERVICE_LOOKUP_GAP(set_core0.pids[i]);
		if (state->audio_request == AUDIO_REQUEST_CLOSE) {
			barrier();
			state->audio_request = AUDIO_REQUEST_CLOSE_QUIESCED;
			slot_state_lock_release();
			continue;
		}
#ifndef ZZAPP_HOST_TEST
		if (state->audio_request == AUDIO_REQUEST_NONE &&
		    state->audio_open && !state->audio_ahi_route &&
		    state->audio_write_frame && *state->audio_write_frame != 0U) {
			barrier();
			state->audio_first_write_seen = 1U;
		}
#endif
		slot_state_lock_release();
	}
#if ZZAPP_PERF_COUNTERS && !defined(ZZAPP_HOST_TEST)
	zzapp_kernel_perf_service_end(zzapp_runtime_kernel(),
				      ZZAPP_PERF_SERVICE_AUDIO,
				      scheduler_perf_stamp());
#endif
	ctx_restore(saved);
}

/*
 * Core 1 finishes a cancelled op. LOAD_BEGIN can be cancelled after core 1
 * takes its deferred record but before the loader owns an arena; its abort
 * then returns EBUSY. Either way the loader drops what it holds and the
 * claim ends through the retire path, which returns the tables and holds the
 * shell. A release that was waiting for the op goes ahead once it is idle.
 */
static void complete_cancelled_deferred(zzapp_rec_t *rec)
{
	struct zzapp_deferred *defer = &runtime_of(rec)->defer;
	uint32_t pid = rec->hdr.pid;
	int release;

	slot_state_lock_acquire();
	release = defer->cancelled && defer->state == DEFER_RUNNING;
	slot_state_lock_release();
	if (!release)
		return;
	ctx_pid[service_context()] = pid;
	(void)zzapp_loader_abort();
	(void)zzapp_rec_retire(pid);
	slot_state_lock_acquire();
	if (defer->cancelled && defer->state == DEFER_RUNNING) {
		defer->start_pending = 0U;
		defer->state = DEFER_IDLE;
		finish_shell_release(rec);
	}
	slot_state_lock_release();
}

static void discard_deferred_run(zzapp_rec_t *rec)
{
	struct zzapp_deferred *defer = &runtime_of(rec)->defer;
	int cancelled;

	slot_state_lock_acquire();
	cancelled = defer->cancelled;
	if (!cancelled && defer->state == DEFER_RUNNING)
		defer->state = DEFER_IDLE;
	slot_state_lock_release();
	if (cancelled)
		complete_cancelled_deferred(rec);
}

static int finish_deferred(zzapp_rec_t *rec, int rc, uint32_t publish_state)
{
	struct zzapp_service_runtime *state = runtime_of(rec);
	struct zzapp_deferred *defer = &state->defer;

#ifdef ZZAPP_SERVICES_STANDALONE_TEST
	if (service_test_deferred_completing_hook)
		service_test_deferred_completing_hook();
#endif
	slot_state_lock_acquire();
	if (defer->cancelled || defer->state != DEFER_RUNNING) {
		slot_state_lock_release();
		complete_cancelled_deferred(rec);
		return 0;
	}
	defer->status = zz_to_sdk(rc);
	if (rc != AAR_OK) {
		defer->reply_len = 0U;
		/* A refused load names the reason and, for the codes the status
		 * cannot tell apart, the AAR_E_* code (ZZAPP_REFUSAL_CODE). */
		if (defer->opcode == ZZAPP_OP_LOAD_BEGIN ||
		    defer->opcode == ZZAPP_OP_LOAD_END) {
			uint32_t code = (rc == AAR_E_INVAL || rc == AAR_E_NOMEM ||
					 rc == AAR_E_PROFILE || rc == AAR_E_NOGROUP)
				? (uint32_t)-rc : 0U;

			if (rec->claim.refusal_reason != AAR_REASON_NONE || code) {
				zzapp_put_be32(defer->reply + ZZAPP_REFUSAL_REASON,
					       rec->claim.refusal_reason);
				defer->reply_len = ZZAPP_REFUSAL_REPLY_BYTES;
			}
			if (code) {
				zzapp_put_be32(defer->reply + ZZAPP_REFUSAL_CODE,
					       code);
				defer->reply_len = ZZAPP_REFUSAL_CODE_REPLY_BYTES;
			}
		}
	}
	if (publish_state == ZZAPP_ST_LOADING)
		state->load_progress = now_us();
	if (publish_state != UINT32_MAX)
		set_state_locked(rec, publish_state);
#ifdef ZZAPP_SERVICES_STANDALONE_TEST
	if (service_test_deferred_reply_hook) {
		slot_state_lock_release();
		service_test_deferred_reply_hook();
		slot_state_lock_acquire();
		if (defer->cancelled || defer->state != DEFER_RUNNING) {
			slot_state_lock_release();
			complete_cancelled_deferred(rec);
			return 0;
		}
	}
#endif
	__atomic_store_n(&defer->state, DEFER_DONE, __ATOMIC_RELEASE);
	slot_state_lock_release();
	return 1;
}

static void run_deferred(zzapp_rec_t *rec)
{
	struct zzapp_service_runtime *state = runtime_of(rec);
	struct zzapp_deferred *defer = &state->defer;
	int rc = AAR_E_INVAL;
	const char *reject_reason = NULL;
	const uint8_t *req = defer->req;
	uint16_t len = defer->req_len;
	uint32_t publish_state = UINT32_MAX;
	int end_claim = 0;

	if (defer_state_load(defer) != DEFER_RUNNING) {
		complete_cancelled_deferred(rec);
		return;
	}
	defer->reply_len = 0U;
	if (defer_cancelled(defer)) {
		discard_deferred_run(rec);
		return;
	}
	switch (defer->opcode) {
	case ZZAPP_OP_LOAD_BEGIN: {
		zzapp_xport_buf_t load;

		if (rec->claim.state != ZZAPP_ST_IDLE &&
		    rec->claim.state != ZZAPP_ST_CLAIMED) {
			rc = AAR_E_BUSY;
			break;
		}
		if (defer_state_load(defer) != DEFER_RUNNING || defer_cancelled(defer)) {
			discard_deferred_run(rec);
			return;
		}
		rc = zzapp_loader_set_generation(aar_handle_generation(rec->hdr.handle));
		if (rc != AAR_OK) {
			if (rc == AAR_E_INVAL)
				reject_reason = "loader arena remains bound";
			break;
		}
		/* The load buffer (staging, then argv) lives until START has
		 * read argv, or the claim ends. */
		rc = zzapp_xport_block_alloc(rec, ZZAPP_XPORT_BLOCK_LOAD,
					     ZZAPP_LOAD_BUFFER_BYTES, &load);
		if (rc != AAR_OK) {
			end_claim = 1;
			break;
		}
		rc = zzapp_loader_begin();
		if (rc != AAR_OK) {
			(void)zzapp_xport_block_free(rec, ZZAPP_XPORT_BLOCK_LOAD);
			if (rc == AAR_E_NOMEM || rc == AAR_E_NORESOURCE)
				/* The staging block could not be had: the claim
				 * ends and its record and tables go back. */
				end_claim = 1;
			break;
		}
		publish_state = ZZAPP_ST_LOADING;
		rec->claim.load_step = 0U;
		zzapp_xport_cb_put(rec, AAR_CB_PROGRESS_LO, 0U);
		zzapp_xport_cb_put(rec, AAR_CB_PROGRESS_HI, 0U);
		zzapp_put_be32(defer->reply + ZZAPP_LOAD_REPLY_TOKEN, load.token);
		zzapp_put_be32(defer->reply + ZZAPP_LOAD_REPLY_SIZE, load.bytes);
		zzapp_put_be32(defer->reply + ZZAPP_LOAD_REPLY_CHUNK_MAX,
			       ZZAPP_CHUNK_MAX);
		zzapp_put_be32(defer->reply + ZZAPP_LOAD_REPLY_ARGV_OFF,
			       ZZAPP_LOAD_ARGV_OFF);
		defer->reply_len = ZZAPP_LOAD_BEGIN_REPLY_BYTES;
		break;
	}
	case ZZAPP_OP_LOAD_CHUNK: {
		zzapp_xport_buf_t load;
		uint32_t off;
		uint32_t n;

		if (len < ZZAPP_CHUNK_REQ_BYTES) {
			reject_reason = "chunk payload is shorter than 8 bytes";
			rc = AAR_E_INVAL;
			break;
		}
		off = zzapp_get_be32(req + ZZAPP_CHUNK_FILE_OFF);
		n = zzapp_get_be32(req + ZZAPP_CHUNK_LENGTH);
		if (n < ZZAPP_CHUNK_MIN || n > ZZAPP_CHUNK_MAX) {
			reject_reason = "chunk length is outside staging bounds";
			rc = AAR_E_INVAL;
			break;
		}
		if (rec->claim.state != ZZAPP_ST_LOADING ||
		    !zzapp_xport_block_get(rec, ZZAPP_XPORT_BLOCK_LOAD, &load)) {
			reject_reason = "chunk received outside loading state";
			rc = AAR_E_INVAL;
			break;
		}
		rc = zzapp_loader_chunk(off, load.ptr, n);
		if (rc == AAR_OK) {
			slot_state_lock_acquire();
			if (!defer_cancelled(defer) &&
			    rec->claim.state == ZZAPP_ST_LOADING)
				state->load_progress = now_us();
			slot_state_lock_release();
		}
		if (rc == AAR_E_INVAL)
			reject_reason = "loader rejected chunk offset or range";
		break;
	}
	case ZZAPP_OP_LOAD_END:
		if (rec->claim.state != ZZAPP_ST_LOADING) {
			reject_reason = "load end received outside loading state";
			rc = AAR_E_INVAL;
			break;
		}
		rc = zzapp_loader_end();
		if (rc == ZZAPP_LOADER_IN_PROGRESS) {
			uint32_t step;

			slot_state_lock_acquire();
			state->load_progress = now_us();
			step = ++rec->claim.load_step;
			slot_state_lock_release();
			zzapp_xport_cb_put(rec, AAR_CB_PROGRESS_LO, step);
			zzapp_xport_cb_put(rec, AAR_CB_PROGRESS_HI, 0U);
			return;
		}
		if (rc == AAR_E_PROFILE)
			reject_reason = "note profile is not A32";
		else if (rc == AAR_E_NOGROUP)
			reject_reason = "note requires a group this runtime lacks";
		if (rc == AAR_OK)
			/* The card address of image offset 0, which fault_pc less
			 * load_base is relative to. */
			zzapp_xport_cb_put64(rec, AAR_CB_LOAD_BASE_HI,
					     zzapp_loader_card());
		/* A failed LOAD_END ends the claim: the loader dropped its
		 * frames and staging, and the record goes through retire. */
		if (rc != AAR_OK)
			end_claim = 1;
		break;
	case ZZAPP_OP_START:
		if (rec->claim.state != ZZAPP_ST_LOADING &&
		    rec->claim.state != ZZAPP_ST_STARTING) {
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
		capture_start_name(rec, defer->start_argc);
		if (defer_state_load(defer) != DEFER_RUNNING || defer_cancelled(defer)) {
			discard_deferred_run(rec);
			return;
		}
		defer->start_pending = 1U;
		return;
	default:
		rc = AAR_E_NOSYS;
		break;
	}
	if ((zz_to_sdk(rc) == SDK_STATUS_BAD_REQUEST || rc == AAR_E_NOGROUP) &&
	    reject_reason)
		report_rejected_app_op(defer->opcode, reject_reason);
#ifdef ZZAPP_SERVICES_STANDALONE_TEST
	if (service_test_deferred_completion_hook)
		service_test_deferred_completion_hook();
#endif
	if (end_claim) {
		/* The reason is in the control block before the op completes. */
		if (rec->claim.refusal_reason == AAR_REASON_NONE &&
		    (rc == AAR_E_NOMEM || rc == AAR_E_NORESOURCE))
			zzapp_services_note_refusal(AAR_REASON_MEMORY);
		(void)zzapp_rec_retire(rec->hdr.pid);
	}
	(void)finish_deferred(rec, rc, publish_state);
}

/* Core 1, the system thread: runs the next posted deferred op, round robin
 * over the records that have one. */
static zzapp_rec_t *pick_deferred(void)
{
	zzapp_rec_t *found = NULL;
	uint32_t k;

	slot_state_lock_acquire();
	for (k = 0U; k < set_core0.count; k++) {
		uint32_t idx = (deferred_poll_cursor + k) % set_core0.count;
		zzapp_rec_t *rec = zzapp_rec_by_pid(set_core0.pids[idx]);
		struct zzapp_deferred *defer;

		if (!rec)
			continue;
		defer = &runtime_of(rec)->defer;
		if (defer->state == DEFER_POSTED ||
		    (defer->state == DEFER_RUNNING &&
		     defer->opcode == ZZAPP_OP_LOAD_END)) {
			defer->state = DEFER_RUNNING;
			deferred_poll_cursor = idx + 1U;
			found = rec;
			break;
		}
	}
	slot_state_lock_release();
	return found;
}

void zzapp_services_poll(void)
{
	struct svc_ctx saved = ctx_save();

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
		zzapp_rec_t *rec = pick_deferred();

		if (rec) {
			ctx_pid[service_context()] = rec->hdr.pid;
			run_deferred(rec);
		}
		ctx_restore(saved);
	}
	service_pass();
	audio_core1_poll_all();
}

int zzapp_services_take_start(uint32_t *argc)
{
	zzapp_rec_t *rec = service_rec();
	struct zzapp_deferred *defer;

	if (!rec)
		return 0;
	defer = &runtime_of(rec)->defer;
	if (!defer->start_pending)
		return 0;
	defer->start_pending = 0U;
	if (argc)
		*argc = defer->start_argc;
	return 1;
}

int zzapp_services_start_entered(uint32_t generation)
{
	zzapp_rec_t *rec = service_rec();
	struct zzapp_deferred *defer;

	if (!rec)
		return AAR_E_IO;
	defer = &runtime_of(rec)->defer;
	slot_state_lock_acquire();
	if (defer->cancelled ||
	    generation != aar_handle_generation(rec->hdr.handle) ||
	    defer->state != DEFER_RUNNING || defer->opcode != ZZAPP_OP_START) {
		slot_state_lock_release();
		complete_cancelled_deferred(rec);
		return AAR_E_IO;
	}
	set_state_locked(rec, ZZAPP_ST_RUNNING);
	defer->status = SDK_STATUS_OK;
	defer->reply_len = 0U;
	__atomic_store_n(&defer->state, DEFER_DONE, __ATOMIC_RELEASE);
	slot_state_lock_release();
	return AAR_OK;
}

void zzapp_services_cancel_start(int status)
{
	zzapp_rec_t *rec = service_rec();
	struct zzapp_deferred *defer;

	if (!rec)
		return;
	defer = &runtime_of(rec)->defer;
	slot_state_lock_acquire();
	defer->start_pending = 0U;
	if (defer->state == DEFER_RUNNING && defer->opcode == ZZAPP_OP_START) {
		defer->status = zz_to_sdk(status);
		defer->reply_len = 0U;
		__atomic_store_n(&defer->state, DEFER_DONE, __ATOMIC_RELEASE);
		/* Admission stays closed until the host's retire holds the
		 * shell. */
		if (!zzapp_xport_flag_test(rec, ZZAPP_XPORT_F_QUARANTINED))
			rec->claim.state = ZZAPP_ST_STOPPING;
	}
	slot_state_lock_release();
}

static int queue_deferred(zzapp_rec_t *rec, uint16_t opcode,
			  const uint8_t *req, uint16_t req_len,
			  uint32_t request_id, uint32_t user_cookie)
{
	struct zzapp_deferred *defer = &runtime_of(rec)->defer;

	slot_state_lock_acquire();
	if (zzapp_rec_state(rec->hdr.pid) != ZZAPP_REC_LIVE ||
	    defer->state != DEFER_IDLE) {
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
	defer->reply_len = 0U;
	/* The limits and the capabilities belong to the request that was
	 * admitted: a LOAD_BEGIN refused above, or one the claim is no longer in
	 * a state to start, leaves the first load's alone. */
	if (opcode == ZZAPP_OP_LOAD_BEGIN &&
	    (rec->claim.state == ZZAPP_ST_IDLE ||
	     rec->claim.state == ZZAPP_ST_CLAIMED)) {
		uint32_t host_limit;
		uint32_t weight;

		rec->claim.client_caps =
			zzapp_get_be32(defer->req + ZZAPP_LOAD_CLIENT_CAPS);
		rec->claim.memory_limit =
			zzapp_xport_cb_read_client(rec, AAR_CB_PRIVATE_MIB) *
			1024U * 1024U;
		host_limit = zzapp_xport_cb_read_client(rec, AAR_CB_SHARED_MIB) *
			     1024U * 1024U;
		rec->claim.host_visible_limit = host_limit;
		zzapp_xport_set_shared_limit(rec, host_limit != 0U ? host_limit :
					     ZZAPP_HOST_VISIBLE_DEFAULT_BYTES);
		/* The admission above refused a weight over the bound. The
		 * word is the client's and can change after it, so a late
		 * change is held to the bound rather than let into the
		 * scheduler. */
		weight = zzapp_xport_cb_read_client(rec, AAR_CB_WEIGHT);
		rec->claim.weight = weight == 0U ? 1U :
			weight > ZZAPP_WEIGHT_MAX ? ZZAPP_WEIGHT_MAX : weight;
	}
	defer->state = DEFER_POSTED;
	set_add(&set_core0, rec->hdr.pid);
	slot_state_lock_release();
	ring_core1();
	return 1;
}

/* Bytes committed by every record that holds memory. */
static uint32_t committed_bytes(void)
{
	uint32_t pid;
	uint32_t total = 0U;

	for (pid = 0U; pid < zzapp_rec_capacity(); pid++) {
		zzapp_rec_t *rec;

		if (zzapp_rec_state(pid) == ZZAPP_REC_FREE)
			continue;
		rec = zzapp_rec_by_pid(pid);
		if (rec)
			total += rec->proc.memory.committed;
	}
	return total;
}

/* Free and largest free bytes of the host-visible heap, the pool the pairs
 * and service buffers come from. */
static void hostheap_stats(uint32_t *free_total, uint32_t *largest)
{
#ifdef ZZAPP_HOST_TEST
	*free_total = zzapp_test_hostheap_free ? zzapp_test_hostheap_free() : 0U;
	*largest = zzapp_test_hostheap_largest ? zzapp_test_hostheap_largest() :
		*free_total;
#else
	*free_total = 0U;
	*largest = 0U;
	surface_allocator_free_stats(free_total, largest);
#endif
}

static void put_query(uint8_t *reply, uint16_t *reply_len, uint32_t caps)
{
	uint32_t committed = committed_bytes();
	uint32_t commit_free;
	uint32_t heap_free;
	uint32_t heap_largest;
	zzapp_rec_stats_t records;
	zzapp_as_stats_t tables;

	memset(reply, 0, ZZAPP_MAILBOX_PAYLOAD_BYTES);
	zzapp_put_be32(reply + ZZAPP_QUERY_ABI_MAJOR, ZZAPP_ABI_MAJOR);
	zzapp_put_be32(reply + ZZAPP_QUERY_ABI_MINOR, ZZAPP_ABI_MINOR);
	zzapp_put_be32(reply + ZZAPP_QUERY_CAPS, caps);
	*reply_len = ZZAPP_MAILBOX_PAYLOAD_BYTES;
	if (caps == 0U)
		return;
	zzapp_put_be32(reply + ZZAPP_QUERY_EVENTS, zzapp_xport_events_token());
	commit_free = zzapp_loader_pool_free ? zzapp_loader_pool_free() : 0U;
	zzapp_rec_stats(&records);
	zzapp_as_stats(&tables);
	hostheap_stats(&heap_free, &heap_largest);
	zzapp_put_be32(reply + ZZAPP_QUERY_HOSTHEAP_LARGEST, heap_largest);
	zzapp_put_be32(reply + ZZAPP_QUERY_QUEUE_DEPTH, ZZAPP_XPORT_DEPTH);
	zzapp_put_be32(reply + ZZAPP_QUERY_COMMIT_FREE, commit_free);
	zzapp_put_be32(reply + ZZAPP_QUERY_COMMIT_TOTAL, commit_free + committed);
	zzapp_put_be32(reply + ZZAPP_QUERY_RECORDS_FREE, records.free);
	/* Tables a new admission can still reserve (free and not promised),
	 * and the pool less the kernel template. */
	zzapp_put_be32(reply + ZZAPP_QUERY_TABLES_FREE, tables.l2_shared_free);
	zzapp_put_be32(reply + ZZAPP_QUERY_TABLES_TOTAL,
		       tables.l2_total - tables.l2_template);
	zzapp_put_be32(reply + ZZAPP_QUERY_HOSTHEAP_FREE, heap_free);
}

/*
 * Claim admission (design 3.2). Steps in order, each undone by the ones after
 * it on a refusal, and nothing published until the last: handle plus record
 * (MEMORY), L2 reserve (TABLES), the pair block (HOSTHEAP). Returns
 * AAR_REASON_NONE or the reason. The record becomes LIVE last, after the
 * control block is initialised and a release barrier.
 */
static uint32_t claim_record(uint32_t *handle_out, uint32_t *token,
			     uint32_t *cb_bytes)
{
	zzapp_rec_t *rec = NULL;
	zzapp_xport_buf_t pair;
	uint32_t handle = 0U;
	uint32_t reason;
	uint32_t pid;

	xport_env_ensure();
	slot_state_lock_acquire();
	reason = zzapp_rec_admit(&handle, &rec);
	slot_state_lock_release();
	if (reason != AAR_REASON_NONE)
		return reason;
	pid = rec->hdr.pid;
	/* The pair comes from the allocator, which has its own lock. */
	reason = zzapp_xport_admit(rec);
	if (reason != AAR_REASON_NONE) {
		slot_state_lock_acquire();
		(void)zzapp_rec_unadmit(handle);
		slot_state_lock_release();
		return reason;
	}
	slot_state_lock_acquire();
	zzapp_kernel_reset_cpu_ticks(zzapp_runtime_kernel(), pid);
	rec->claim.state = ZZAPP_ST_CLAIMED;
	rec->claim.memory_limit = 0U;
	rec->claim.host_visible_limit = 0U;
	rec->claim.weight = 1U;
	rec->claim.terminal_reason = ZZAPP_END_NONE;
	rec->claim.refusal_reason = AAR_REASON_NONE;
	runtime_of(rec)->claim_started = now_us();
	runtime_of(rec)->load_progress = 0U;
	runtime_of(rec)->lease_seen =
		zzapp_xport_cb_read_client(rec, AAR_CB_LEASE_COUNTER);
	zzapp_xport_cb_put(rec, AAR_CB_LEASE_PERIOD_MS,
			   (uint32_t)(ZZAPP_CLAIM_LEASE_US / 1000U));
	if (zzapp_host_claimed)
		zzapp_host_claimed(pid);
	if (zzapp_host_reset_low_memory)
		zzapp_host_reset_low_memory(pid);
	set_add(&set_core0, pid);
	(void)zzapp_rec_set_state(rec, ZZAPP_REC_LIVE);
	slot_state_lock_release();
	(void)zzapp_xport_block_get(rec, ZZAPP_XPORT_BLOCK_PAIR, &pair);
	*handle_out = handle;
	*token = pair.token;
	*cb_bytes = aar_pair_bytes(ZZAPP_XPORT_DEPTH);
	return AAR_REASON_NONE;
}

/* The next live claim at or after `cursor`. */
static void put_list(uint8_t *reply, uint16_t *reply_len, uint32_t cursor)
{
	zzapp_kernel_t *kernel = zzapp_runtime_kernel();
	zzapp_rec_t *rec = NULL;
	uint32_t cap = zzapp_rec_capacity();
	uint32_t pid;
	uint64_t cpu = 0U;
	uint64_t service = 0U;
	uint8_t block = ZZAPP_BLOCK_NONE;

	memset(reply, 0, ZZAPP_LIST_REPLY_BYTES);
	*reply_len = ZZAPP_LIST_REPLY_BYTES;
	slot_state_lock_acquire();
	for (pid = cursor; pid < cap; pid++) {
		zzapp_rec_t *r;

		if (zzapp_rec_state(pid) == ZZAPP_REC_FREE)
			continue;
		r = zzapp_rec_by_pid(pid);
		if (r && zzapp_xport_live(r)) {
			rec = r;
			break;
		}
	}
	if (!rec) {
		slot_state_lock_release();
		return;
	}
	cpu = rec->thread.user_ticks + rec->thread.svc_ticks;
	if (kernel)
		service = kernel->service_ticks;
	zzapp_put_be32(reply + ZZAPP_LIST_STATE, rec->claim.state);
	zzapp_put_be32(reply + ZZAPP_LIST_HANDLE, rec->hdr.handle);
	zzapp_put_be32(reply + ZZAPP_LIST_COMMITTED, rec->proc.memory.committed);
	zzapp_put_be32(reply + ZZAPP_LIST_INDEX, rec->hdr.pid);
	zzapp_put_be32(reply + ZZAPP_LIST_CPU_MS,
		       cpu > UINT32_MAX ? UINT32_MAX : (uint32_t)cpu);
	zzapp_put_be32(reply + ZZAPP_LIST_SERVICE_MS,
		       service > UINT32_MAX ? UINT32_MAX : (uint32_t)service);
	reply[ZZAPP_LIST_REASONS + 1U] = (uint8_t)rec->claim.terminal_reason;
	if (rec->thread.state == ZZAPP_THREAD_SLEEPING) {
		block = ZZAPP_BLOCK_SLEEP;
	} else if (rec->thread.state == ZZAPP_THREAD_BLOCKED) {
		switch (runtime_of(rec)->wait_kind) {
		case ZZAPP_WAIT_FILE:
		case ZZAPP_WAIT_SQ_SPACE:
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
	memcpy(reply + ZZAPP_LIST_NAME, runtime_of(rec)->name,
	       ZZAPP_LIST_NAME_BYTES);
	if (kernel && kernel->overload_count != 0U)
		zzapp_put_be16(reply + ZZAPP_LIST_REASONS + 2U,
			       ZZAPP_LIST_FLAG_OVERLOAD);
	slot_state_lock_release();
}

/*
 * A request that names a handle which is not the live one: if its index
 * holds a quarantined shell, the old client has found out and the shell goes
 * (design 3.10). 1 when a shell was released or is queued to be.
 */
static int release_stale_quarantine(uint32_t handle)
{
	uint32_t pid = aar_handle_index(handle);
	zzapp_rec_t *rec = NULL;
	int found = 0;

	if (pid >= zzapp_rec_capacity())
		return 0;
	slot_state_lock_acquire();
	if (zzapp_rec_state(pid) != ZZAPP_REC_FREE)
		rec = zzapp_rec_by_pid(pid);
	if (rec && zzapp_xport_flag_test(rec, ZZAPP_XPORT_F_QUARANTINED)) {
		zzapp_xport_flag_set(rec, ZZAPP_XPORT_F_RELEASE_REQUESTED);
		finish_shell_release(rec);
		found = 1;
	}
	slot_state_lock_release();
	return found;
}

/*
 * STOP of a claim. A claim that nothing was loaded into ends here through the
 * one retire path; one whose load is running is cancelled and core 1 ends it;
 * anything else gets the stop latch.
 */
static void stop_record(zzapp_rec_t *rec)
{
	uint32_t pid = rec->hdr.pid;

	slot_state_lock_acquire();
	if (rec->claim.state == ZZAPP_ST_CLAIMED) {
		int release_now = stop_current_deferred(rec);

		if (release_now == 0) {
			begin_claim_cancellation(rec, ZZAPP_END_FORCED);
			slot_state_lock_release();
			return;
		}
		if (release_now > 0) {
			/* Nothing was loaded and core 1 has no stake: the claim
			 * ends here through the one retire path. */
			rec->claim.memory_limit = 0U;
			rec->claim.host_visible_limit = 0U;
			rec->claim.terminal_reason = ZZAPP_END_FORCED;
			slot_state_lock_release();
			(void)zzapp_rec_retire(pid);
			return;
		}
	}
	/* RUNNING reads STOPPING from the stop request on (spec 5.1); the
	 * teardown ends in IDLE. */
	if (rec->claim.state == ZZAPP_ST_RUNNING) {
		rec->claim.state = ZZAPP_ST_STOPPING;
		cb_publish_state(rec, ZZAPP_ST_STOPPING);
	}
	zzapp_host_latch_stop(pid);
	slot_state_lock_release();
}

/*
 * RELEASE: the normal close of a claim. A claim that still runs is stopped
 * and core 1's retire frees it (the pair may be in use by a pump on core 1);
 * a shell that is already reclaimed is freed here, when nothing is owed to
 * core 0 (an audio handoff) or to the client (an unharvested reply).
 */
static void release_record(zzapp_rec_t *rec)
{
	slot_state_lock_acquire();
	zzapp_xport_flag_set(rec, ZZAPP_XPORT_F_RELEASE_REQUESTED);
	if (zzapp_rec_state(rec->hdr.pid) == ZZAPP_REC_RECLAIMED) {
		finish_shell_release(rec);
		slot_state_lock_release();
		return;
	}
	slot_state_lock_release();
	if (zzapp_rec_state(rec->hdr.pid) == ZZAPP_REC_LIVE)
		stop_record(rec);
}

static int record_op(zzapp_rec_t *rec, uint16_t opcode, const uint8_t *req,
		     uint16_t req_len, uint32_t request_id, uint32_t user_cookie)
{
	switch (opcode) {
	case ZZAPP_OP_STOP:
		stop_record(rec);
		return SDK_STATUS_OK;
	case ZZAPP_OP_RELEASE:
		release_record(rec);
		return SDK_STATUS_OK;
	case ZZAPP_OP_KICK: {
		uint32_t flags = zzapp_get_be32(req + ZZAPP_KICK_FLAGS);

		if ((flags & ~ZZAPP_KICK_KNOWN) != 0U) {
			report_rejected_app_op(opcode, "kick flag unknown");
			return SDK_STATUS_BAD_REQUEST;
		}
		if (zzapp_xport_kick(rec->hdr.handle, flags) != AAR_OK)
			return SDK_STATUS_BAD_HANDLE;
		slot_state_lock_acquire();
		set_add(&set_svc, rec->hdr.pid);
		slot_state_lock_release();
		ring_core1();
		return SDK_STATUS_OK;
	}
	case ZZAPP_OP_LOAD_BEGIN: {
		uint32_t caps = zzapp_get_be32(req + ZZAPP_LOAD_CLIENT_CAPS);
		uint32_t mib = zzapp_xport_cb_read_client(rec, AAR_CB_PRIVATE_MIB);
		uint32_t host_mib = zzapp_xport_cb_read_client(rec, AAR_CB_SHARED_MIB);
		uint32_t weight = zzapp_xport_cb_read_client(rec, AAR_CB_WEIGHT);

		if ((caps & ~ZZAPP_CLIENT_KNOWN) != 0U) {
			report_rejected_app_op(opcode, "client capability unknown");
			return SDK_STATUS_BAD_REQUEST;
		}
		if (mib > UINT32_MAX / (1024U * 1024U) ||
		    host_mib > UINT32_MAX / (1024U * 1024U)) {
			report_rejected_app_op(opcode, "load limit above 4095 MiB");
			return SDK_STATUS_BAD_REQUEST;
		}
		if (weight > ZZAPP_WEIGHT_MAX) {
			report_rejected_app_op(opcode, "weight above 256");
			return SDK_STATUS_BAD_REQUEST;
		}
		/* The limit is a cap, not a reservation: only a request the
		 * heap and the app's alias range could never hold is refused.
		 * Allocations can still fail below the cap. */
		if (host_mib > zzapp_host_visible_capacity() / (1024U * 1024U)) {
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
			report_rejected_app_op(opcode,
				"deferred operation has request id zero");
			return SDK_STATUS_BAD_REQUEST;
		}
		if (!scheduler_core1_available())
			return SDK_STATUS_UNSUPPORTED;
		if (!queue_deferred(rec, opcode, req, req_len, request_id,
				    user_cookie))
			return SDK_STATUS_BUSY;
		return SDK_STATUS_QUEUED;
	default:
		return SDK_STATUS_UNSUPPORTED;
	}
}

uint16_t zzapp_mailbox_dispatch(uint16_t opcode, const uint8_t *req,
				uint16_t req_len, uint8_t *reply,
				uint16_t *reply_len, int z3,
				uint32_t request_id, uint32_t user_cookie)
{
	struct svc_ctx saved;
	zzapp_rec_t *rec;
	uint32_t handle;
	int rc;

	if (!reply || !reply_len)
		return SDK_STATUS_BAD_REQUEST;
	*reply_len = 0U;
	if (opcode < ZZAPP_OP_FIRST || opcode > ZZAPP_OP_RANGE_HI)
		return SDK_STATUS_UNSUPPORTED;
	if (!z3) {
		if (opcode == ZZAPP_OP_QUERY)
			put_query(reply, reply_len, 0U);
		return SDK_STATUS_UNSUPPORTED;
	}
	if (opcode == ZZAPP_OP_QUERY) {
		put_query(reply, reply_len,
			  ZZAPP_CAP_Z3 | ZZAPP_CAP_DISPLAY | ZZAPP_CAP_AUDIO |
			  ZZAPP_CAP_GL | ZZAPP_CAP_CLUT8);
		return SDK_STATUS_OK;
	}
	if (opcode == ZZAPP_OP_CLAIM) {
		uint32_t token = 0U;
		uint32_t cb_bytes = 0U;
		uint32_t reason = claim_record(&handle, &token, &cb_bytes);

		if (reason != AAR_REASON_NONE) {
			zzapp_put_be32(reply + ZZAPP_REFUSAL_REASON, reason);
			*reply_len = ZZAPP_REFUSAL_REPLY_BYTES;
			return reason == AAR_REASON_LIMIT ? SDK_STATUS_BUSY :
							    SDK_STATUS_NO_MEMORY;
		}
		zzapp_put_be32(reply + ZZAPP_CLAIM_HANDLE, handle);
		zzapp_put_be32(reply + ZZAPP_CLAIM_CB_TOKEN, token);
		zzapp_put_be32(reply + ZZAPP_CLAIM_CB_BYTES, cb_bytes);
		*reply_len = ZZAPP_CLAIM_REPLY_BYTES;
		return SDK_STATUS_OK;
	}
	if (opcode == ZZAPP_OP_LIST) {
		if (!req || req_len < ZZAPP_LIST_CURSOR + 4U)
			return SDK_STATUS_BAD_REQUEST;
		put_list(reply, reply_len,
			 zzapp_get_be32(req + ZZAPP_LIST_CURSOR));
		return SDK_STATUS_OK;
	}
	if (opcode != ZZAPP_OP_LOAD_BEGIN && opcode != ZZAPP_OP_LOAD_CHUNK &&
	    opcode != ZZAPP_OP_LOAD_END && opcode != ZZAPP_OP_START &&
	    opcode != ZZAPP_OP_STOP && opcode != ZZAPP_OP_RELEASE &&
	    opcode != ZZAPP_OP_KICK)
		return SDK_STATUS_UNSUPPORTED;
	if (!req || req_len != ZZAPP_MAILBOX_PAYLOAD_BYTES)
		return SDK_STATUS_BAD_REQUEST;
	handle = zzapp_get_be32(req + ZZAPP_SEL_HANDLE);
	rec = zzapp_xport_resolve(handle, &rc);
	if (!rec) {
		/* Nothing changes for a stale handle, except that a
		 * quarantined shell is released when its old client turns up
		 * with a request whose answer it reads before it touches the
		 * claim again: STOP and RELEASE. A KICK is posted without
		 * waiting for the status, and a deferred load call is answered
		 * while the client's lease tick may still write the control
		 * block, so neither proves the client has let go of its
		 * mappings; the client follows a stale deferred reply with a
		 * RELEASE of its own. */
		if (opcode == ZZAPP_OP_STOP || opcode == ZZAPP_OP_RELEASE)
			(void)release_stale_quarantine(handle);
		return SDK_STATUS_BAD_HANDLE;
	}
	saved = ctx_save();
	ctx_pid[service_context()] = rec->hdr.pid;
	rc = record_op(rec, opcode, req, req_len, request_id, user_cookie);
	ctx_restore(saved);
	return (uint16_t)rc;
}

int zzapp_mailbox_harvest(uint32_t *request_id, uint32_t *user_cookie,
			  uint16_t *opcode, uint16_t *status,
			  uint8_t *payload, uint16_t *payload_len)
{
	struct zzapp_deferred *defer;
	uint32_t i;

	slot_state_lock_acquire();
	if (deferred_harvest_pid == ZZAPP_PID_NONE) {
		for (i = 0U; i < set_core0.count; i++) {
			zzapp_rec_t *rec = zzapp_rec_by_pid(set_core0.pids[i]);

			if (rec && __atomic_load_n(&runtime_of(rec)->defer.state,
						   __ATOMIC_ACQUIRE) == DEFER_DONE) {
				deferred_harvest_pid = set_core0.pids[i];
				break;
			}
		}
		if (deferred_harvest_pid == ZZAPP_PID_NONE) {
			slot_state_lock_release();
			return 0;
		}
	}
	defer = &runtime_of(zzapp_rec_by_pid(deferred_harvest_pid))->defer;
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
	if (deferred_harvest_pid != ZZAPP_PID_NONE) {
		zzapp_rec_t *rec = zzapp_rec_by_pid(deferred_harvest_pid);

		if (rec && runtime_of(rec)->defer.state == DEFER_DONE) {
			runtime_of(rec)->defer.state = DEFER_IDLE;
			/* A release that waited for this reply goes ahead. */
			finish_shell_release(rec);
		}
	}
	deferred_harvest_pid = ZZAPP_PID_NONE;
	slot_state_lock_release();
}

#ifdef ZZAPP_HOST_TEST
/* Hard reset between sub-tests: the service statics and the hooks go back to
 * their initial state. The records are the test's to release
 * (zzapp_xport_release, zzapp_rec_free). */
void zzapp_services_test_reset(void)
{
	set_clear(&set_svc);
	set_clear(&set_core0);
	set_clear(&set_display);
	ctx_pid[0] = ZZAPP_PID_NONE;
	ctx_pid[1] = ZZAPP_PID_NONE;
	deferred_harvest_pid = ZZAPP_PID_NONE;
	deferred_poll_cursor = 0U;
	display_mailbox_pid = ZZAPP_PID_NONE;
	display_mailbox_state = NULL;
	defer_test_core = 1;
	yield_hook = NULL;
	time_hook = NULL;
#ifdef ZZAPP_SERVICES_STANDALONE_TEST
	service_test_current_thread = -1;
	service_test_wake_count = 0U;
	service_stub_direct_audio = 0U;
	service_loader_generation = 0U;
	service_test_loader_set_generation_hook = NULL;
	service_test_loader_begin_hook = NULL;
	service_test_loader_begin_rc = AAR_OK;
	service_test_loader_chunk_hook = NULL;
	service_test_loader_end_hook = NULL;
	service_test_deferred_completion_hook = NULL;
	service_test_deferred_completing_hook = NULL;
	service_test_deferred_reply_hook = NULL;
	service_test_lookup_hook = NULL;
	service_test_loader_abort_count = 0U;
#endif
	memset(slot_busy_test, 0, sizeof(slot_busy_test));
}

void zzapp_services_test_set_core(int core)
{
	defer_test_core = core;
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

/* The services part of the selected record, still readable after the record
 * was released: records are type stable, a freed one keeps its bytes until
 * the index is admitted again. */
static struct zzapp_service_runtime *test_runtime(void)
{
	return runtime_of(zzapp_rec_by_pid(service_pid()));
}

void zzapp_services_test_set_audio_request(uint32_t request)
{
	slot_state_lock_acquire();
	test_runtime()->audio_request = request;
	set_add(&set_core0, service_pid());
	slot_state_lock_release();
}

uint32_t zzapp_services_test_audio_request(void)
{
	return test_runtime()->audio_request;
}

uint32_t zzapp_services_test_audio_detach_count(void)
{
	return test_runtime()->audio_detach_count;
}

/* The fabric's underrun callback only exists on the card. */
void zzapp_services_test_audio_underrun(void)
{
	audio_note_underrun(test_runtime());
}
#endif
