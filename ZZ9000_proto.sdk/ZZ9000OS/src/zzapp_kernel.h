/*
 * Core-1 app scheduler policy.
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ZZAPP_KERNEL_H
#define ZZAPP_KERNEL_H

#include <stdint.h>

#ifndef ZZAPP_PERF_COUNTERS
#define ZZAPP_PERF_COUNTERS 1
#endif

/* The most records the scheduler can address: handles carry a 16-bit index
 * and the table is bounded at 1024 (zzapp_rec.h). The capacity in use is
 * the one zzapp_kernel_init() was given. */
#define ZZAPP_KERNEL_MAX_APPS 1024U
/* "No process": active thread, API handoff and TLB sampling sentinel. The
 * same value as in zzapp_rec.h, so it can never collide with a real pid. */
#define ZZAPP_PID_NONE 0xffffffffU
/* Entries of the wake log ring; independent of the record count. */
#define ZZAPP_KERNEL_WAKE_LOG 16U
#define ZZAPP_KERNEL_QUANTUM_MS 5U
/* Virtual run time is charged in microseconds times this factor, divided by
 * the app's weight. 2^16 leaves a microsecond at weight 256 a value of 256,
 * and a weight-1 app takes about nine years of continuous run time to wrap
 * the 64-bit counter. */
#define ZZAPP_KERNEL_VRUNTIME_SCALE 65536ULL
/* The most a woken thread may be behind the floor: one quantum of run time at
 * weight 1, not divided by the thread's weight. A weight-w thread comes back
 * up to w quanta of its own run time behind, so it may run that long before
 * a neighbour is ahead of it. A credit of one quantum at the thread's own
 * weight is smaller than the quantum of a weight-1 neighbour: a weight-8 app
 * that blocks for 0.1 ms hands such a neighbour a whole 5 ms quantum, and
 * the next wake clamps away the 5 ms it ran ahead, so the heavy app lost a
 * quantum every other wake (model: 66.7 % of the core against an 8/9 share,
 * 1333 clamps in 2666 wakes; with this credit 88.9 % and none). */
#define ZZAPP_KERNEL_WAKE_CREDIT \
	((uint64_t)ZZAPP_KERNEL_QUANTUM_MS * 1000U * ZZAPP_KERNEL_VRUNTIME_SCALE)
/* Wake granularity: the active app keeps the core until a runnable thread
 * lags it by more than 1 ms of that thread's own run time. The wake rule
 * gives a waker up to ZZAPP_KERNEL_WAKE_CREDIT, so a thread
 * that was parked, or that mostly waits on a client, is behind by more than
 * this when it wakes and runs at once instead of after the rest of the active
 * app's quantum. CPU-bound peers lag each other by what one of them ran since
 * the last pick, so they swap at most once per millisecond of run time and the
 * switch rate stays bounded. The limit is in virtual run time at weight 1 and
 * is scaled by the lagging thread's weight, so a heavy interactive app is not
 * held back by a light spinner. */
#define ZZAPP_KERNEL_WAKE_GRAN_US 1000U
#define ZZAPP_KERNEL_WAKE_GRAN \
	((uint64_t)ZZAPP_KERNEL_WAKE_GRAN_US * ZZAPP_KERNEL_VRUNTIME_SCALE)
#define ZZAPP_KERNEL_SERVICE_WINDOW_MS 100U
#define ZZAPP_KERNEL_SERVICE_LIMIT_MS 95U
/* Covers the 13,216-byte MP3 decode scratch, exception frame, and callers. */
#define ZZAPP_KERNEL_STACK_BYTES (32U << 10)
#define ZZAPP_KERNEL_PAGE_BYTES 4096U
#define ZZAPP_KERNEL_SESSION_LIMIT 4U
#define ZZAPP_KERNEL_APP_CPSR 0x10U
#define ZZAPP_KERNEL_WAIT_CANCELLED (-1)
#define ZZAPP_KERNEL_WAIT_TIMEOUT (-8)
#define ZZAPP_KERNEL_SYSTEM_OWNER (-2)

#if ZZAPP_PERF_COUNTERS
#define ZZAPP_PERF_HISTOGRAM_BUCKETS 32U

typedef enum {
	ZZAPP_PERF_SERVICE_SDK,
	ZZAPP_PERF_SERVICE_MINIGL,
	ZZAPP_PERF_SERVICE_LAUNCHER,
	ZZAPP_PERF_SERVICE_AUDIO,
	ZZAPP_PERF_SERVICE_SVC,
	ZZAPP_PERF_SERVICE_COUNT
} zzapp_perf_service_t;

typedef enum {
	ZZAPP_PERF_LAUNCHER_ZERO,
	ZZAPP_PERF_LAUNCHER_COPY,
	ZZAPP_PERF_LAUNCHER_MAP,
	ZZAPP_PERF_LAUNCHER_RELOC,
	ZZAPP_PERF_LAUNCHER_APPLY,
	ZZAPP_PERF_LAUNCHER_ALLOC,
	ZZAPP_PERF_LAUNCHER_OTHER,
	ZZAPP_PERF_LAUNCHER_STEP_COUNT
} zzapp_perf_launcher_step_t;

typedef struct {
	uint32_t samples;
	uint32_t max;
	uint32_t histogram[ZZAPP_PERF_HISTOGRAM_BUCKETS];
} zzapp_kernel_perf_metric_t;

typedef struct {
	zzapp_kernel_perf_metric_t switch_cycles;
	zzapp_kernel_perf_metric_t sgi_to_service;
	zzapp_kernel_perf_metric_t sdk_enqueue_to_start;
	zzapp_kernel_perf_metric_t service[ZZAPP_PERF_SERVICE_COUNT];
	uint32_t service_max_detail[ZZAPP_PERF_SERVICE_COUNT];
	uint32_t service_max_bytes[ZZAPP_PERF_SERVICE_COUNT];
	uint32_t launcher_step_max[ZZAPP_PERF_LAUNCHER_STEP_COUNT];
	uint32_t sgis;
	uint32_t work_publications;
	uint32_t audio_low_water;
	uint32_t audio_starved;
	uint32_t audio_underruns;
	uint32_t tlb_instruction_refills;
	uint32_t tlb_data_refills;
} zzapp_kernel_perf_snapshot_t;

typedef struct {
	zzapp_kernel_perf_snapshot_t total;
	uint32_t service_start[ZZAPP_PERF_SERVICE_COUNT];
	uint32_t sgi_stamp;
	uint32_t last_report_ms;
	uint32_t report_count;
	/* Records with a report request pending, and how many of those are
	 * forced. The flags themselves live in the records. */
	uint32_t report_pending;
	uint32_t report_force;
	uint32_t report_ready;
	uint32_t sgi_pending;
	uint32_t tlb_instruction_stamp;
	uint32_t tlb_data_stamp;
	/* The record whose TLB refills are being sampled, or ZZAPP_PID_NONE. */
	uint32_t tlb_pid;
	uint64_t tlb_sample_until_ms;
	uint32_t tlb_stamps_valid;
	char report_text[640];
} zzapp_kernel_perf_t;
#endif

typedef enum {
	ZZAPP_KERNEL_DISPATCH_TICK = 0,
	ZZAPP_KERNEL_DISPATCH_DOORBELL,
	ZZAPP_KERNEL_DISPATCH_VOLUNTARY,
	ZZAPP_KERNEL_DISPATCH_INTERRUPT
} zzapp_kernel_dispatch_t;

/*
 * Exact stack layout made by zzapp_kernel_asm.S, in ascending addresses:
 * interrupted SYS sp/lr, SPSR, d16-d31, d0-d15, FPSCR, r0-r12, exception LR.
 * The vector returns through exception_lr, subtracting its exception-specific
 * return adjustment.
 */
typedef struct {
	uint32_t user_sp;
	uint32_t user_lr;
	uint32_t spsr;
	uint32_t vfp[64];
	uint32_t fpscr;
	uint32_t r[13];
	uint32_t exception_lr;
} zzapp_arm_frame_t;
/* FRAME_BYTES in zzapp_kernel_asm.S copies this struct. */
_Static_assert(sizeof(zzapp_arm_frame_t) == 328, "exception frame size");

/* The exception handler uses this scratch stack below frame while saving
 * the current context. */
typedef struct {
	uint32_t exception_stack[sizeof(zzapp_arm_frame_t) / sizeof(uint32_t)];
	zzapp_arm_frame_t frame;
} zzapp_thread_context_t;


typedef enum {
	ZZAPP_THREAD_STOPPED = 0,
	ZZAPP_THREAD_RUNNABLE,
	ZZAPP_THREAD_BLOCKED,
	ZZAPP_THREAD_SLEEPING
} zzapp_thread_state_t;

typedef enum {
	ZZAPP_WAKE_NONE = 0,
	ZZAPP_WAKE_READY,
	ZZAPP_WAKE_CANCELLED,
	ZZAPP_WAKE_TIMEOUT
} zzapp_wake_reason_t;

typedef enum {
	ZZAPP_KERNEL_IDLE = -2,
	ZZAPP_KERNEL_SYSTEM = -1
} zzapp_kernel_pick_t;

#if ZZAPP_PERF_COUNTERS
/* Park causes of the scheduling counters: 0 is a block with no wait
 * registered (the API lock), 1..9 the ZZAPP_WAIT_* kind the services
 * registered with zzapp_kernel_park_kind, ZZAPP_KERNEL_PARK_SLEEP a sleep. */
#define ZZAPP_KERNEL_PARK_SLEEP 10U
#define ZZAPP_KERNEL_PARK_KINDS 11U

/* Why the last pick chose its thread. */
typedef enum {
	ZZAPP_PICK_NONE = 0,
	ZZAPP_PICK_SYSTEM,	/* service work first */
	ZZAPP_PICK_HANDOFF,	/* API lock handed to a waiter */
	ZZAPP_PICK_OWNER,	/* the API lock owner finishes its call */
	ZZAPP_PICK_STICKY,	/* the active thread keeps its quantum */
	ZZAPP_PICK_LAG,		/* a thread lags the active one: wake preemption */
	ZZAPP_PICK_MIN,		/* smallest virtual run time, none active */
	ZZAPP_PICK_KINDS
} zzapp_pick_reason_t;

/* Where a preempted app's core time went to: it was runnable and lost the
 * core to this. */
enum {
	ZZAPP_PREEMPT_SYSTEM = 0,
	ZZAPP_PREEMPT_LAG,
	ZZAPP_PREEMPT_QUANTUM,	/* quantum end or a voluntary switch */
	ZZAPP_PREEMPT_OTHER,	/* API lock handoff or owner */
	ZZAPP_PREEMPT_KINDS
};

/* One app's scheduling counters, since its start. Times are microseconds of
 * zzapp_kernel_now_us. The thread is in exactly one state: on the core (RUN,
 * counted in run_us), runnable behind another context (WAIT, counted in
 * wait_sys_us while the system thread has the core and wait_app_us while an
 * app has it, from snapshots of the kernel's two running totals) or parked
 * (PARK, counted per cause). */
typedef struct {
	uint8_t state;
	uint8_t park_kind;	/* cause of the park in progress */
	uint8_t next_kind;	/* cause of the next park, from the services */
	uint8_t reserved;
	uint32_t picks;		/* times a dispatch gave the app the core */
	uint64_t since_us;	/* start of the run or park in progress */
	uint64_t snap_sys_us;	/* kernel totals when the wait began */
	uint64_t snap_app_us;
	uint64_t run_us;
	uint64_t wait_sys_us;
	uint64_t wait_app_us;
	uint64_t park_us[ZZAPP_KERNEL_PARK_KINDS];
	uint32_t park_n[ZZAPP_KERNEL_PARK_KINDS];
	uint32_t wakes;
	uint32_t clamps;	/* wakes that raised the virtual run time */
	uint64_t clamp_vr;	/* virtual run time those wakes discarded */
	uint32_t preempt[ZZAPP_PREEMPT_KINDS];
	uint32_t deferrals;	/* service held back while this app was picked */
	/* Dispatch decisions that gave the app the core, by ZZAPP_PICK_*; the
	 * app that keeps the core counts too. */
	uint32_t why[ZZAPP_PICK_KINDS];
	uint32_t yields;	/* yields the app made */
	uint32_t yields_given;	/* of those, the ones a peer was entitled to */
	uint32_t yield_skips;	/* picks that passed the app over for a yield
				 * while its virtual run time was the smallest */
	uint32_t hold_skips;	/* picks that kept the active app although this
				 * one was behind it, by less than the wake
				 * granularity */
} zzapp_sched_acct_t;
#endif

typedef struct {
	zzapp_thread_state_t state;
	zzapp_wake_reason_t wake_reason;
	uint8_t yield_pending;
	/* 1 while the pid is on the kernel's run list (not STOPPED). */
	uint8_t in_run;
	uint32_t quantum_left;
	uint64_t user_ticks;
	uint64_t svc_ticks;
	/* Millisecond of the kernel clock a sleep ends at (zzapp_kernel_sleep). */
	uint64_t sleep_deadline;
	/* Microsecond of zzapp_kernel_now_us() an aar_sleep_us ends at, or 0:
	 * the sleeper wakes when the clock reaches it, never before. */
	uint64_t sleep_deadline_us;
	uint32_t wake_sequence;
	uint32_t stack_high_water;
	/* CPU share weight of the claim, 1..ZZAPP_WEIGHT_MAX; 0 counts as 1. */
	uint32_t weight;
	/* Virtual run time: charged microseconds scaled by
	 * ZZAPP_KERNEL_VRUNTIME_SCALE over the weight. The runnable thread with
	 * the smallest value runs next. */
	uint64_t vruntime;
#if ZZAPP_PERF_COUNTERS
	zzapp_sched_acct_t acct;
#endif
	zzapp_thread_context_t context;
} zzapp_thread_t;

typedef struct {
	uint32_t surface_base;
	uint32_t surface_bytes;
	uint32_t surface_generation;
	uint32_t gl_generation;
	uint32_t direct_audio;
	uint32_t image_sessions;
	uint32_t audio_sessions;
} zzapp_kernel_resources_t;

/*
 * The scheduler. Threads are not owned by it: they are the first member of
 * the process records, so `threads` is the record area and `stride` the
 * distance between records. Per-record kernel words (resources, API wait
 * state, perf counters) sit in the same record at offsetof(zzapp_rec_t,
 * kernel). Nothing here is sized by the record count except `run`.
 */
typedef struct {
	uint8_t *threads;
	uint32_t stride;
	uint32_t capacity;
	/* Dense list of the pids whose thread is not STOPPED, ascending. Every
	 * scan walks it, not the capacity. Changes only at start and stop,
	 * with local IRQs masked. */
	uint32_t run_count;
	uint16_t run[ZZAPP_KERNEL_MAX_APPS];
	zzapp_thread_context_t system_context;
	uint32_t system_context_valid;
	uint64_t now;
	uint64_t service_ticks;
	uint32_t overload_count;
	/* Threads of equal virtual run time take turns: the next pick among
	 * them starts at the first runnable pid >= rr_next. */
	uint32_t rr_next;
	/* The thread whose yield is in force (its yield_pending is set), until a
	 * pick gives the core to an app. */
	uint32_t yielder;
	uint32_t active;
	uint32_t service_due;
	uint64_t window_start;
	uint32_t service_in_window;
	/* Microsecond clock reading at the start of the interval not yet
	 * charged to a context. */
	uint64_t slice_started_us;
	uint32_t slice_valid;
	/* Monotonic lower bound of the virtual run time of the runnable
	 * threads: the reference of a start or a wake when none is runnable. */
	uint64_t vfloor;
#if ZZAPP_PERF_COUNTERS
	/* Microseconds the core spent in the system thread and in apps, up to
	 * slice_started_us (the idle core is neither). */
	uint64_t acct_sys_us;
	uint64_t acct_app_us;
	uint32_t acct_at_boundary; /* use committed totals during dispatch */
	zzapp_pick_reason_t pick_reason;
	uint32_t defer_pending;
	/* The app that held the core last, whatever the system thread did since. */
	uint32_t acct_last;
#endif
	uint32_t idle;
	uint64_t idle_ticks;
	uint32_t wake_serial;
	/* Ring of the latest wakes; wake_log_count counts all of them. */
	uint16_t wake_log[ZZAPP_KERNEL_WAKE_LOG];
	uint32_t wake_log_count;
	int api_lock_owner;
	uint32_t api_lock_depth;
	uint32_t api_handoff;
	uint32_t api_handoff_service_deferred;
	uint32_t api_wait_serial;
	int gl_owner;
	uint32_t gl_generation;
	uint32_t direct_audio_count;
#if ZZAPP_PERF_COUNTERS
	zzapp_kernel_perf_t perf;
#endif
} zzapp_kernel_t;

/*
 * Attach the scheduler to a record area: `threads` is the first record,
 * `stride` the bytes between records (at least sizeof(zzapp_rec_t)), at most
 * ZZAPP_KERNEL_MAX_APPS of them. Every thread starts STOPPED and every
 * per-record kernel word is cleared. 0, or -1 for a bad area.
 */
int zzapp_kernel_init(zzapp_kernel_t *kernel, void *threads, uint32_t stride,
		      uint32_t capacity);
/*
 * A scheduler with no process records, for a card that booted without a
 * record arena: capacity 0, no thread to run, but the system thread's kernel
 * lock and service passes work, so the shared worker (SDK, media, MiniGL)
 * still runs. Every record lookup answers NULL.
 */
void zzapp_kernel_init_system_only(zzapp_kernel_t *kernel);
/* The thread of record `pid`, or NULL outside the capacity. */
zzapp_thread_t *zzapp_kernel_thread(const zzapp_kernel_t *kernel, uint32_t pid);
uint32_t zzapp_kernel_capacity(const zzapp_kernel_t *kernel);
/* The run list: pids whose thread is not STOPPED, ascending. */
uint32_t zzapp_kernel_run_count(const zzapp_kernel_t *kernel);
uint32_t zzapp_kernel_run_pid(const zzapp_kernel_t *kernel, uint32_t index);
/* Bytes of the record's kernel stack that were touched since it was last
 * painted (prepare_app paints it), 0 outside the capacity. */
uint32_t zzapp_kernel_stack_high_water(const zzapp_kernel_t *kernel,
				       uint32_t pid);

void zzapp_kernel_reset_cpu_ticks(zzapp_kernel_t *kernel, uint32_t pid);
/* Sets the CPU share weight of `pid`: 1..ZZAPP_WEIGHT_MAX, or 0 for the
 * default of 1. -1 for a larger value or a pid outside the capacity. Call it
 * before zzapp_kernel_start_app: a thread never given a weight counts as 1. */
int zzapp_kernel_set_weight(zzapp_kernel_t *kernel, uint32_t pid,
			    uint32_t weight);
/* Puts the thread on the run list at the smallest virtual run time of the
 * runnable threads (the monotonic floor when none is runnable). */
int zzapp_kernel_start_app(zzapp_kernel_t *kernel, uint32_t pid);
/* Names the cause of the next park of `pid` for the scheduling counters
 * (a ZZAPP_WAIT_* kind of the services, 0 for none). It is used by the next
 * block and then forgotten. Without ZZAPP_PERF_COUNTERS it does nothing. */
void zzapp_kernel_park_kind(zzapp_kernel_t *kernel, uint32_t pid,
			    uint32_t kind);
/* Builds the first context of `pid` and paints its kernel stack. */
int zzapp_kernel_prepare_app(zzapp_kernel_t *kernel, uint32_t pid,
			      uint32_t entry, uint32_t api, uint32_t argc,
			      uint32_t argv, uint32_t stack_top);
int zzapp_kernel_dispatch(zzapp_kernel_t *kernel,
			 zzapp_kernel_dispatch_t source, uint32_t elapsed_ms);
int zzapp_kernel_wait_status(const zzapp_kernel_t *kernel, uint32_t pid);
int zzapp_kernel_apps_runnable(const zzapp_kernel_t *kernel);
zzapp_arm_frame_t *zzapp_kernel_switch_frame(zzapp_kernel_t *kernel,
					      zzapp_arm_frame_t *frame,
					      zzapp_kernel_dispatch_t source,
					      uint32_t return_adjust);
/* Runtime glue in zzapp_host.c, called by the SVC vector after an API
 * call: returns the selected return frame. */
zzapp_arm_frame_t *zzapp_kernel_svc_return(zzapp_arm_frame_t *frame);
int zzapp_kernel_block(zzapp_kernel_t *kernel, uint32_t pid);
/* Atomically release API lock, snapshot now, and enter SLEEPING under IRQ
 * mask. The sleep lasts at least `sleep_us` microseconds of
 * zzapp_kernel_now_us() and ends at the first look at the clock after that
 * (the thread's sleep_deadline_us); the millisecond deadline is the first
 * boundary at or after the reading plus `sleep_us`. */
int zzapp_kernel_api_sleep(zzapp_kernel_t *kernel, uint32_t pid, uint64_t sleep_us);
/* Wakes the sleepers past their microsecond deadline; 1 when one is due
 * within `horizon_us`. For the system thread's loop. */
int zzapp_kernel_sleep_poll(zzapp_kernel_t *kernel, uint32_t horizon_us);
int zzapp_kernel_wake(zzapp_kernel_t *kernel, uint32_t pid,
		zzapp_wake_reason_t reason);
int zzapp_kernel_sleep(zzapp_kernel_t *kernel, uint32_t pid,
		uint64_t deadline);
void zzapp_kernel_advance(zzapp_kernel_t *kernel, uint32_t elapsed_ms);
void zzapp_kernel_service_due(zzapp_kernel_t *kernel);
/* The calling thread goes behind its runnable peers for the next pick. */
void zzapp_kernel_yield(zzapp_kernel_t *kernel);
/* Microseconds since boot. Firmware supplies the global timer; tests inject
 * one. The scheduler charges run time from this clock at its resolution. */
uint64_t zzapp_kernel_now_us(void);
uint64_t zzapp_kernel_now(const zzapp_kernel_t *kernel);
void zzapp_kernel_idle_begin(zzapp_kernel_t *kernel);
void zzapp_kernel_idle_end(zzapp_kernel_t *kernel);
void zzapp_kernel_reset_slice(zzapp_kernel_t *kernel);
int zzapp_kernel_schedule(zzapp_kernel_t *kernel, uint32_t elapsed_ms);

#if ZZAPP_PERF_COUNTERS
void zzapp_kernel_perf_sgi_raised(zzapp_kernel_t *kernel, uint32_t stamp);
void zzapp_kernel_perf_sgi_started(zzapp_kernel_t *kernel, uint32_t stamp);
void zzapp_kernel_perf_work_published(zzapp_kernel_t *kernel, uint32_t count);
void zzapp_kernel_perf_sdk_started(zzapp_kernel_t *kernel, uint32_t enqueue_stamp,
				   uint32_t stamp);
void zzapp_kernel_perf_service_begin(zzapp_kernel_t *kernel,
				      zzapp_perf_service_t service,
				      uint32_t stamp);
void zzapp_kernel_perf_service_end(zzapp_kernel_t *kernel,
				    zzapp_perf_service_t service,
				    uint32_t stamp);
void zzapp_kernel_perf_service_end_detail(zzapp_kernel_t *kernel,
					   zzapp_perf_service_t service,
					   uint32_t stamp, uint32_t detail,
					   uint32_t bytes);
void zzapp_kernel_perf_audio(zzapp_kernel_t *kernel, uint32_t pid,
			      uint32_t low_water, uint32_t starved,
			      uint32_t underruns);
void zzapp_kernel_perf_finish_slot(zzapp_kernel_t *kernel, uint32_t pid);
void zzapp_kernel_perf_snapshot(const zzapp_kernel_t *kernel, uint32_t pid,
				 zzapp_kernel_perf_snapshot_t *snapshot);
/* Ask for a report of `pid`: a per-record flag, not a mask bit, so it works
 * for every pid. */
void zzapp_kernel_perf_request_report(zzapp_kernel_t *kernel, uint32_t pid,
				       int force);
int zzapp_kernel_perf_emit_reports(zzapp_kernel_t *kernel);
void zzapp_kernel_perf_print_reports(zzapp_kernel_t *kernel);
/* The scheduling counters of `pid` as of now, the span in progress included.
 * 0, or -1 outside the capacity. */
int zzapp_kernel_sched_snapshot(const zzapp_kernel_t *kernel, uint32_t pid,
				zzapp_sched_acct_t *out);
/* Prints the app's "[app] sched" line (docs/app-runtime.md). */
void zzapp_kernel_sched_report(zzapp_kernel_t *kernel, uint32_t pid);
void zzapp_kernel_perf_switch_finished(zzapp_kernel_t *kernel);
extern volatile uint32_t zzapp_kernel_switch_entry_stamp;
extern volatile uint32_t zzapp_kernel_switch_exit_stamp;
extern volatile uint32_t zzapp_kernel_switch_complete;
extern volatile uint32_t zzapp_kernel_switch_completed_entry_stamp;
/*
 * The SVC vector reads this pointer, not a slot index, to find the counters
 * of the running app. It always points at a block of
 * ZZAPP_SVC_COUNTER_BLOCK_BYTES: the current thread's block in its record
 * (offset 0 of the record's svc part), or a scratch block when no app runs.
 * zzapp_kernel_set_current_thread() publishes it.
 */
#define ZZAPP_SVC_COUNTER_BLOCK_BYTES 184U
extern void *volatile zzapp_svc_counter_block;
#ifdef ZZAPP_HOST_TEST
uint32_t zzapp_kernel_perf_test_percentile(
	const zzapp_kernel_perf_metric_t *metric, uint32_t percentile);
#endif
#endif


int zzapp_kernel_api_lock(zzapp_kernel_t *kernel, uint32_t pid);
int zzapp_kernel_api_unlock(zzapp_kernel_t *kernel, uint32_t pid);
/* The common case of the two above, without their checks of the general case:
 * 1 when it took (or gave) the lock, 0 with nothing changed when the general
 * call is needed. See the comment in zzapp_kernel.c. */
int zzapp_kernel_api_try_lock(zzapp_kernel_t *kernel, uint32_t pid);
int zzapp_kernel_api_try_unlock(zzapp_kernel_t *kernel, uint32_t pid);
int zzapp_kernel_api_block(zzapp_kernel_t *kernel, uint32_t pid);
int zzapp_kernel_system_lock(zzapp_kernel_t *kernel);
int zzapp_kernel_system_unlock(zzapp_kernel_t *kernel);
int zzapp_kernel_api_fault_release(zzapp_kernel_t *kernel, uint32_t pid);
/* Core 0 calls this only while core 1 is held in reset. */
void zzapp_kernel_reset_lock(zzapp_kernel_t *kernel);
int zzapp_kernel_surface_bind(zzapp_kernel_t *kernel, uint32_t pid,
			      uint32_t generation, uint32_t base, uint32_t bytes);
void zzapp_kernel_surface_unbind(zzapp_kernel_t *kernel, uint32_t pid);
int zzapp_kernel_audio_open(zzapp_kernel_t *kernel, uint32_t pid,
			    int direct_card_route);
void zzapp_kernel_audio_close(zzapp_kernel_t *kernel, uint32_t pid);
int zzapp_kernel_gl_bind(zzapp_kernel_t *kernel, uint32_t pid,
			 uint32_t generation);
void zzapp_kernel_gl_unbind(zzapp_kernel_t *kernel, uint32_t pid,
			    uint32_t generation);
int zzapp_kernel_session_open(zzapp_kernel_t *kernel, uint32_t pid,
			      int audio_session);
void zzapp_kernel_session_close(zzapp_kernel_t *kernel, uint32_t pid,
			       int audio_session);
/* Takes the pid off the run list and marks the thread STOPPED. */
void zzapp_kernel_stop_app(zzapp_kernel_t *kernel, uint32_t pid);

/* The exception path identifies an app by its scheduled thread, not by the
 * faulting PC. `pid` is a record index; -1 (system) and -2 (idle) are the
 * only negative values. */
void zzapp_kernel_set_current_thread(int pid);
/* Published by the scheduler before an app returns to user mode; the SVC
 * vector reads it directly instead of calling back into C on entry. */
extern volatile uintptr_t zzapp_kernel_svc_stack_top;
int zzapp_kernel_current_thread(void);
int zzapp_kernel_svc_can_return(void);
void zzapp_kernel_charge_svc(zzapp_kernel_t *kernel, uint32_t pid,
		uint32_t elapsed_ms);
void zzapp_kernel_stack_paint(uint8_t *stack, uint32_t bytes);
uint32_t zzapp_kernel_stack_used(const uint8_t *stack, uint32_t bytes);

#endif
