/*
 * Core-1 app scheduler policy.
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ZZAPP_KERNEL_H
#define ZZAPP_KERNEL_H

#include <stdint.h>

#define ZZAPP_KERNEL_APP_SLOTS 4U
#define ZZAPP_KERNEL_QUANTUM_MS 5U
#define ZZAPP_KERNEL_SERVICE_WINDOW_MS 100U
#define ZZAPP_KERNEL_SERVICE_LIMIT_MS 95U
#define ZZAPP_KERNEL_STACK_BYTES 8192U
#define ZZAPP_KERNEL_PAGE_BYTES 4096U
#define ZZAPP_KERNEL_SESSION_LIMIT 4U
#define ZZAPP_KERNEL_APP_CPSR 0x1fU
#define ZZAPP_KERNEL_WAIT_CANCELLED (-1)
#define ZZAPP_KERNEL_WAIT_TIMEOUT (-8)

typedef enum {
	ZZAPP_KERNEL_DISPATCH_TICK = 0,
	ZZAPP_KERNEL_DISPATCH_DOORBELL,
	ZZAPP_KERNEL_DISPATCH_VOLUNTARY
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

/* Scheduler-visible context. r15 is the interrupted or first-entry PC. */
typedef struct {
	uint32_t r[16];
	uint32_t cpsr;
	uint32_t fpscr;
	uint64_t d[32];
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
	uint32_t wake_serial;
	uint8_t wake_log[ZZAPP_KERNEL_APP_SLOTS];
	uint32_t wake_log_count;
	int api_lock_owner;
	uint32_t api_lock_depth;
	int gl_owner;
	uint32_t gl_generation;
	uint32_t direct_audio_count;
	zzapp_kernel_resources_t resources[ZZAPP_KERNEL_APP_SLOTS];
} zzapp_kernel_t;

void zzapp_kernel_init(zzapp_kernel_t *kernel);
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
int zzapp_kernel_block(zzapp_kernel_t *kernel, uint32_t slot);
int zzapp_kernel_wake(zzapp_kernel_t *kernel, uint32_t slot,
		zzapp_wake_reason_t reason);
int zzapp_kernel_sleep(zzapp_kernel_t *kernel, uint32_t slot,
		uint64_t deadline);
void zzapp_kernel_advance(zzapp_kernel_t *kernel, uint32_t elapsed_ms);
void zzapp_kernel_service_due(zzapp_kernel_t *kernel);
int zzapp_kernel_schedule(zzapp_kernel_t *kernel, uint32_t elapsed_ms);

int zzapp_kernel_api_lock(zzapp_kernel_t *kernel, uint32_t slot);
int zzapp_kernel_api_unlock(zzapp_kernel_t *kernel, uint32_t slot);
int zzapp_kernel_api_block(zzapp_kernel_t *kernel, uint32_t slot);
int zzapp_kernel_surface_bind(zzapp_kernel_t *kernel, uint32_t slot,
			      uint32_t generation, uint32_t base, uint32_t bytes);
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
int zzapp_kernel_current_thread(void);
void zzapp_kernel_charge_svc(zzapp_kernel_t *kernel, uint32_t slot,
		uint32_t elapsed_ms);
void zzapp_kernel_stack_paint(uint8_t *stack, uint32_t bytes);
uint32_t zzapp_kernel_stack_used(const uint8_t *stack, uint32_t bytes);

#endif
