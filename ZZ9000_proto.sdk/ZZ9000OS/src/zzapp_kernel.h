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

#define ZZAPP_KERNEL_APP_SLOTS 4U
#define ZZAPP_KERNEL_QUANTUM_MS 5U
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
	uint32_t report_pending;
	uint32_t report_force;
	uint32_t report_ready;
	uint32_t sgi_pending;
	uint32_t tlb_instruction_stamp;
	uint32_t tlb_data_stamp;
	uint32_t tlb_slot;
	uint64_t tlb_sample_until_ms;
	uint32_t tlb_stamps_valid;
	char report_text[640];
	uint32_t audio_low_water[ZZAPP_KERNEL_APP_SLOTS];
	uint32_t audio_starved[ZZAPP_KERNEL_APP_SLOTS];
	uint32_t audio_underruns[ZZAPP_KERNEL_APP_SLOTS];
	uint32_t tlb_instruction_refills[ZZAPP_KERNEL_APP_SLOTS];
	uint32_t tlb_data_refills[ZZAPP_KERNEL_APP_SLOTS];
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
	ZZAPP_KERNEL_SYSTEM = -1,
	ZZAPP_KERNEL_APP0 = 0,
	ZZAPP_KERNEL_APP1,
	ZZAPP_KERNEL_APP2,
	ZZAPP_KERNEL_APP3
} zzapp_kernel_pick_t;

typedef struct {
	zzapp_thread_state_t state;
	zzapp_wake_reason_t wake_reason;
	uint8_t yield_pending;
	uint32_t quantum_left;
	uint64_t user_ticks;
	uint64_t svc_ticks;
	uint64_t sleep_deadline;
	uint32_t wake_sequence;
	uint32_t stack_high_water;
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

typedef struct {
	zzapp_thread_t threads[ZZAPP_KERNEL_APP_SLOTS];
	zzapp_thread_context_t system_context;
	uint32_t system_context_valid;
	uint64_t now;
	uint64_t service_ticks;
	uint32_t overload_count;
	uint32_t rr_next;
	uint32_t active;
	uint32_t service_due;
	uint64_t window_start;
	uint32_t service_in_window;
	uint64_t slice_started_ms;
	uint32_t slice_valid;
	uint32_t idle;
	uint64_t idle_ticks;
	uint32_t wake_serial;
	uint8_t wake_log[ZZAPP_KERNEL_APP_SLOTS];
	uint32_t wake_log_count;
	int api_lock_owner;
	uint32_t api_lock_depth;
	uint32_t api_handoff;
	uint32_t api_handoff_service_deferred;
	uint32_t api_wait_serial;
	uint32_t api_wait_sequence[ZZAPP_KERNEL_APP_SLOTS];
	uint8_t api_waiting[ZZAPP_KERNEL_APP_SLOTS];
	uint8_t api_resume_waiting[ZZAPP_KERNEL_APP_SLOTS];
	int gl_owner;
	uint32_t gl_generation;
	uint32_t direct_audio_count;
	zzapp_kernel_resources_t resources[ZZAPP_KERNEL_APP_SLOTS];
#if ZZAPP_PERF_COUNTERS
	zzapp_kernel_perf_t perf;
#endif
} zzapp_kernel_t;

void zzapp_kernel_init(zzapp_kernel_t *kernel);
void zzapp_kernel_reset_cpu_ticks(zzapp_kernel_t *kernel, uint32_t slot);
int zzapp_kernel_start_app(zzapp_kernel_t *kernel, uint32_t slot);
int zzapp_kernel_prepare_app(zzapp_kernel_t *kernel, uint32_t slot,
			      uint32_t entry, uint32_t api, uint32_t argc,
			      uint32_t argv, uint32_t stack_top);
int zzapp_kernel_dispatch(zzapp_kernel_t *kernel,
			 zzapp_kernel_dispatch_t source, uint32_t elapsed_ms);
int zzapp_kernel_wait_status(const zzapp_kernel_t *kernel, uint32_t slot);
int zzapp_kernel_apps_runnable(const zzapp_kernel_t *kernel);
zzapp_arm_frame_t *zzapp_kernel_switch_frame(zzapp_kernel_t *kernel,
					      zzapp_arm_frame_t *frame,
					      zzapp_kernel_dispatch_t source,
					      uint32_t return_adjust);
/* Runtime glue in zzapp_host.c, called by the SVC vector after an API
 * call: returns the selected return frame. */
zzapp_arm_frame_t *zzapp_kernel_svc_return(zzapp_arm_frame_t *frame);
int zzapp_kernel_block(zzapp_kernel_t *kernel, uint32_t slot);
/* Atomically release API lock, snapshot now, and enter SLEEPING under IRQ mask. */
int zzapp_kernel_api_sleep(zzapp_kernel_t *kernel, uint32_t slot, uint64_t sleep_ms);
int zzapp_kernel_wake(zzapp_kernel_t *kernel, uint32_t slot,
		zzapp_wake_reason_t reason);
int zzapp_kernel_sleep(zzapp_kernel_t *kernel, uint32_t slot,
		uint64_t deadline);
void zzapp_kernel_advance(zzapp_kernel_t *kernel, uint32_t elapsed_ms);
void zzapp_kernel_service_due(zzapp_kernel_t *kernel);
void zzapp_kernel_yield(zzapp_kernel_t *kernel);
/* Milliseconds since boot. Firmware supplies the global timer; tests inject one. */
uint64_t zzapp_kernel_now_ms(void);
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
void zzapp_kernel_perf_audio(zzapp_kernel_t *kernel, uint32_t slot,
			      uint32_t low_water, uint32_t starved,
			      uint32_t underruns);
void zzapp_kernel_perf_finish_slot(zzapp_kernel_t *kernel, uint32_t slot);
void zzapp_kernel_perf_snapshot(const zzapp_kernel_t *kernel, uint32_t slot,
				 zzapp_kernel_perf_snapshot_t *snapshot);
void zzapp_kernel_perf_request_report(zzapp_kernel_t *kernel, uint32_t slot,
				       int force);
int zzapp_kernel_perf_emit_reports(zzapp_kernel_t *kernel);
void zzapp_kernel_perf_print_reports(zzapp_kernel_t *kernel);
void zzapp_kernel_perf_switch_finished(zzapp_kernel_t *kernel);
extern volatile uint32_t zzapp_kernel_switch_entry_stamp;
extern volatile uint32_t zzapp_kernel_switch_exit_stamp;
extern volatile uint32_t zzapp_kernel_switch_complete;
extern volatile uint32_t zzapp_kernel_switch_completed_entry_stamp;
#ifdef ZZAPP_HOST_TEST
uint32_t zzapp_kernel_perf_test_percentile(
	const zzapp_kernel_perf_metric_t *metric, uint32_t percentile);
#endif
#endif


int zzapp_kernel_api_lock(zzapp_kernel_t *kernel, uint32_t slot);
int zzapp_kernel_api_unlock(zzapp_kernel_t *kernel, uint32_t slot);
int zzapp_kernel_api_block(zzapp_kernel_t *kernel, uint32_t slot);
int zzapp_kernel_system_lock(zzapp_kernel_t *kernel);
int zzapp_kernel_system_unlock(zzapp_kernel_t *kernel);
int zzapp_kernel_api_fault_release(zzapp_kernel_t *kernel, uint32_t slot);
/* Core 0 calls this only while core 1 is held in reset. */
void zzapp_kernel_reset_lock(zzapp_kernel_t *kernel);
int zzapp_kernel_surface_bind(zzapp_kernel_t *kernel, uint32_t slot,
			      uint32_t generation, uint32_t base, uint32_t bytes);
void zzapp_kernel_surface_unbind(zzapp_kernel_t *kernel, uint32_t slot);
int zzapp_kernel_audio_open(zzapp_kernel_t *kernel, uint32_t slot,
			    int direct_card_route);
void zzapp_kernel_audio_close(zzapp_kernel_t *kernel, uint32_t slot);
int zzapp_kernel_gl_bind(zzapp_kernel_t *kernel, uint32_t slot,
			 uint32_t generation);
void zzapp_kernel_gl_unbind(zzapp_kernel_t *kernel, uint32_t slot,
			    uint32_t generation);
int zzapp_kernel_session_open(zzapp_kernel_t *kernel, uint32_t slot,
			      int audio_session);
void zzapp_kernel_session_close(zzapp_kernel_t *kernel, uint32_t slot,
			       int audio_session);
void zzapp_kernel_stop_app(zzapp_kernel_t *kernel, uint32_t slot);

/* The exception path identifies an app by its scheduled thread, not by the
 * faulting PC. */
void zzapp_kernel_set_current_thread(int slot);
/* Published by the scheduler before an app returns to user mode; the SVC
 * vector reads it directly instead of calling back into C on entry. */
extern volatile uintptr_t zzapp_kernel_svc_stack_top;
int zzapp_kernel_current_thread(void);
int zzapp_kernel_svc_can_return(void);
void zzapp_kernel_charge_svc(zzapp_kernel_t *kernel, uint32_t slot,
		uint32_t elapsed_ms);
void zzapp_kernel_stack_paint(uint8_t *stack, uint32_t bytes);
uint32_t zzapp_kernel_stack_used(const uint8_t *stack, uint32_t bytes);

#endif
