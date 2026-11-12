/*
 * Core-1 app scheduler policy.
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <string.h>

#include "sdk_smp_lock.h"
#include "zzapp_kernel.h"

#if ZZAPP_PERF_COUNTERS
#include <stdio.h>
#endif

#ifdef ZZAPP_HOST_TEST
uint32_t __attribute__((weak)) smp_local_irq_save(void)
{
	return 0U;
}

void __attribute__((weak)) smp_local_irq_restore(uint32_t state)
{
	(void)state;
}
#endif


static volatile int current_thread = -1;
volatile uintptr_t zzapp_kernel_svc_stack_top;
#if ZZAPP_PERF_COUNTERS
/* The SVC vector reads this directly to index its per-app PMU stamps. */
volatile uint32_t zzapp_svc_counter_slot;
volatile uint32_t zzapp_kernel_switch_entry_stamp;
volatile uint32_t zzapp_kernel_switch_exit_stamp;
volatile uint32_t zzapp_kernel_switch_complete;
volatile uint32_t zzapp_kernel_switch_completed_entry_stamp;


static void kernel_perf_add(zzapp_kernel_perf_metric_t *metric,
			    uint32_t value)
{
	uint32_t bucket = 0U;
	uint32_t original = value;

	while (value > 1U && bucket + 1U < ZZAPP_PERF_HISTOGRAM_BUCKETS) {
		value >>= 1;
		bucket++;
	}
	metric->samples++;
	metric->histogram[bucket]++;
	if (original > metric->max)
		metric->max = original;
}

static uint32_t kernel_perf_percentile(const zzapp_kernel_perf_metric_t *metric,
				       uint32_t percentile)
{
	uint64_t target;
	uint64_t seen = 0U;
	uint32_t bucket;

	if (!metric->samples)
		return 0U;
	target = ((uint64_t)metric->samples * percentile + 99U) / 100U;
	for (bucket = 0U; bucket < ZZAPP_PERF_HISTOGRAM_BUCKETS; bucket++) {
		seen += metric->histogram[bucket];
		if (seen >= target)
			return 1U << bucket;
	}
	return metric->max;
}

#ifdef ZZAPP_HOST_TEST
uint32_t zzapp_kernel_perf_test_percentile(
	const zzapp_kernel_perf_metric_t *metric, uint32_t percentile)
{
	return kernel_perf_percentile(metric, percentile);
}
#endif

static void kernel_perf_tlb_leave(zzapp_kernel_t *kernel, int slot)
{
	zzapp_kernel_perf_t *perf;
	uint32_t instruction;
	uint32_t data;
	uint32_t irq_state;

	if (!kernel)
		return;
	irq_state = smp_local_irq_save();
	perf = &kernel->perf;
	if (slot < 0 || slot >= (int)ZZAPP_KERNEL_APP_SLOTS ||
	    perf->tlb_slot != (uint32_t)slot) {
		smp_local_irq_restore(irq_state);
		return;
	}
#ifdef ZZAPP_HOST_TEST
	instruction = perf->tlb_instruction_stamp;
	data = perf->tlb_data_stamp;
#else
	__asm__ __volatile__("mcr p15, 0, %0, c9, c12, 5" : : "r"(0U));
	__asm__ __volatile__("isb" ::: "memory");
	__asm__ __volatile__("mrc p15, 0, %0, c9, c13, 2" : "=r"(instruction));
	__asm__ __volatile__("mcr p15, 0, %0, c9, c12, 5" : : "r"(1U));
	__asm__ __volatile__("isb" ::: "memory");
	__asm__ __volatile__("mrc p15, 0, %0, c9, c13, 2" : "=r"(data));
#endif
	perf->tlb_instruction_refills[slot] += instruction -
		perf->tlb_instruction_stamp;
	perf->tlb_data_refills[slot] += data - perf->tlb_data_stamp;
	perf->total.tlb_instruction_refills += instruction -
		perf->tlb_instruction_stamp;
	perf->total.tlb_data_refills += data - perf->tlb_data_stamp;
	perf->tlb_slot = ZZAPP_KERNEL_APP_SLOTS;
	smp_local_irq_restore(irq_state);
}

static void kernel_perf_tlb_enter(zzapp_kernel_t *kernel, int slot)
{
	zzapp_kernel_perf_t *perf = &kernel->perf;

	if (slot < 0 || slot >= (int)ZZAPP_KERNEL_APP_SLOTS)
		return;
#ifndef ZZAPP_HOST_TEST
	__asm__ __volatile__("mcr p15, 0, %0, c9, c12, 5" : : "r"(0U));
	__asm__ __volatile__("isb" ::: "memory");
	__asm__ __volatile__("mrc p15, 0, %0, c9, c13, 2" :
			     "=r"(perf->tlb_instruction_stamp));
	__asm__ __volatile__("mcr p15, 0, %0, c9, c12, 5" : : "r"(1U));
	__asm__ __volatile__("isb" ::: "memory");
	__asm__ __volatile__("mrc p15, 0, %0, c9, c13, 2" :
			     "=r"(perf->tlb_data_stamp));
#endif
	perf->tlb_slot = (uint32_t)slot;
}

void zzapp_kernel_perf_sgi_raised(zzapp_kernel_t *kernel, uint32_t stamp)
{
	if (!kernel)
		return;
	kernel->perf.sgi_stamp = stamp;
	kernel->perf.sgi_pending = 1U;
	kernel->perf.total.sgis++;
}

void zzapp_kernel_perf_sgi_started(zzapp_kernel_t *kernel, uint32_t stamp)
{
	if (!kernel || !kernel->perf.sgi_pending)
		return;
	kernel_perf_add(&kernel->perf.total.sgi_to_service,
			stamp - kernel->perf.sgi_stamp);
	kernel->perf.sgi_pending = 0U;
}

void zzapp_kernel_perf_tasks_queued(zzapp_kernel_t *kernel, uint32_t count)
{
	if (kernel)
		kernel->perf.total.queued_tasks += count;
}

void zzapp_kernel_perf_sdk_started(zzapp_kernel_t *kernel,
				   uint32_t enqueue_stamp, uint32_t stamp)
{
	if (!kernel)
		return;
	kernel_perf_add(&kernel->perf.total.sdk_enqueue_to_start,
			stamp - enqueue_stamp);
}

void zzapp_kernel_perf_service_begin(zzapp_kernel_t *kernel,
				      zzapp_perf_service_t service,
				      uint32_t stamp)
{
	if (kernel && service < ZZAPP_PERF_SERVICE_COUNT)
		kernel->perf.service_start[service] = stamp;
}

void zzapp_kernel_perf_service_end(zzapp_kernel_t *kernel,
				    zzapp_perf_service_t service,
				    uint32_t stamp)
{
	if (!kernel || service >= ZZAPP_PERF_SERVICE_COUNT)
		return;
	kernel_perf_add(&kernel->perf.total.service[service], stamp -
			kernel->perf.service_start[service]);
}

void zzapp_kernel_perf_audio(zzapp_kernel_t *kernel, uint32_t slot,
			      uint32_t low_water, uint32_t starved,
			      uint32_t underruns)
{
	if (!kernel || slot >= ZZAPP_KERNEL_APP_SLOTS)
		return;
	kernel->perf.audio_low_water[slot] += low_water;
	kernel->perf.audio_starved[slot] += starved;
	kernel->perf.audio_underruns[slot] += underruns;
}

void zzapp_kernel_perf_snapshot(const zzapp_kernel_t *kernel, uint32_t slot,
				 zzapp_kernel_perf_snapshot_t *snapshot)
{
	if (!kernel || !snapshot || slot >= ZZAPP_KERNEL_APP_SLOTS)
		return;
	*snapshot = kernel->perf.total;
	snapshot->audio_low_water = kernel->perf.audio_low_water[slot];
	snapshot->audio_starved = kernel->perf.audio_starved[slot];
	snapshot->audio_underruns = kernel->perf.audio_underruns[slot];
	snapshot->tlb_instruction_refills =
		kernel->perf.tlb_instruction_refills[slot];
	snapshot->tlb_data_refills = kernel->perf.tlb_data_refills[slot];
}

void zzapp_kernel_perf_finish_slot(zzapp_kernel_t *kernel, uint32_t slot)
{
	if (!kernel || slot >= ZZAPP_KERNEL_APP_SLOTS)
		return;
	kernel_perf_tlb_leave(kernel, (int)slot);
}

void zzapp_kernel_perf_switch_finished(zzapp_kernel_t *kernel)
{
	uint32_t irq_state = smp_local_irq_save();

	if (zzapp_kernel_switch_complete) {
		if (kernel)
			kernel_perf_add(&kernel->perf.total.switch_cycles,
					zzapp_kernel_switch_exit_stamp -
					zzapp_kernel_switch_completed_entry_stamp);
		zzapp_kernel_switch_complete = 0U;
	}
	smp_local_irq_restore(irq_state);
}

void zzapp_kernel_perf_request_report(zzapp_kernel_t *kernel, uint32_t slot,
				       int force)
{
	uint32_t bit;

	if (!kernel || slot >= ZZAPP_KERNEL_APP_SLOTS)
		return;
	if (!force && kernel->perf.report_count &&
	    (uint32_t)kernel->now - kernel->perf.last_report_ms < 10000U)
		return;
	bit = 1U << slot;
	kernel->perf.report_pending |= bit;
	if (force)
		kernel->perf.report_force |= bit;
}

int zzapp_kernel_perf_emit_reports(zzapp_kernel_t *kernel)
{
	static const char *const service_names[ZZAPP_PERF_SERVICE_COUNT] = {
		"sdk_span", "minigl_span", "launcher_span", "audio_span",
		"svc_span"
	};
	uint32_t pending;
	uint32_t force;
	uint32_t slot;
	uint32_t service;
	zzapp_kernel_perf_snapshot_t snapshot = { 0 };
	int offset;

	if (!kernel || kernel->perf.report_ready)
		return 0;
	pending = kernel->perf.report_pending;
	force = kernel->perf.report_force;
	if (!pending)
		return 0;
	for (slot = 0U; slot < ZZAPP_KERNEL_APP_SLOTS; slot++)
		if (force & (1U << slot))
			break;
	if (slot == ZZAPP_KERNEL_APP_SLOTS) {
		if (kernel->perf.report_count &&
		    (uint32_t)kernel->now - kernel->perf.last_report_ms < 10000U)
			return 0;
		for (slot = 0U; slot < ZZAPP_KERNEL_APP_SLOTS; slot++)
			if (pending & (1U << slot))
				break;
	}
	zzapp_kernel_perf_snapshot(kernel, slot, &snapshot);
	kernel->perf.report_pending &= ~(1U << slot);
	kernel->perf.report_force &= ~(1U << slot);
	if ((force & (1U << slot)) == 0U)
		kernel->perf.last_report_ms = (uint32_t)kernel->now;
	kernel->perf.report_count++;
	offset = snprintf(kernel->perf.report_text, sizeof(kernel->perf.report_text),
			  "[app] perf slot=%lu forced=%u switch=%lu/%lu/%lu "
			  "sgi=%lu/%lu/%lu sdk=%lu/%lu/%lu sgi_burst=%lu/%lu "
			  "audio=%lu/%lu/%lu tlb=%lu/%lu",
			  (unsigned long)slot, (force & (1U << slot)) != 0U,
			  (unsigned long)kernel_perf_percentile(
				  &snapshot.switch_cycles, 50U),
			  (unsigned long)kernel_perf_percentile(
				  &snapshot.switch_cycles, 99U),
			  (unsigned long)snapshot.switch_cycles.max,
			  (unsigned long)kernel_perf_percentile(
				  &snapshot.sgi_to_service, 50U),
			  (unsigned long)kernel_perf_percentile(
				  &snapshot.sgi_to_service, 99U),
			  (unsigned long)snapshot.sgi_to_service.max,
			  (unsigned long)kernel_perf_percentile(
				  &snapshot.sdk_enqueue_to_start, 50U),
			  (unsigned long)kernel_perf_percentile(
				  &snapshot.sdk_enqueue_to_start, 99U),
			  (unsigned long)snapshot.sdk_enqueue_to_start.max,
			  (unsigned long)snapshot.sgis,
			  (unsigned long)snapshot.queued_tasks,
			  (unsigned long)snapshot.audio_low_water,
			  (unsigned long)snapshot.audio_starved,
			  (unsigned long)snapshot.audio_underruns,
			  (unsigned long)snapshot.tlb_instruction_refills,
			  (unsigned long)snapshot.tlb_data_refills);
	for (service = 0U; service < ZZAPP_PERF_SERVICE_COUNT &&
	     offset > 0 && (size_t)offset < sizeof(kernel->perf.report_text);
	     service++)
		offset += snprintf(kernel->perf.report_text + offset,
				   sizeof(kernel->perf.report_text) - (size_t)offset,
				   " %s=%lu/%lu", service_names[service],
				   (unsigned long)kernel_perf_percentile(
					   &snapshot.service[service], 99U),
				   (unsigned long)snapshot.service[service].max);
	if (offset > 0 && (size_t)offset < sizeof(kernel->perf.report_text) - 2U)
		strcat(kernel->perf.report_text, "\r\n");
	kernel->perf.report_ready = 1U;
	return 1;
}

void zzapp_kernel_perf_print_reports(zzapp_kernel_t *kernel)
{
	if (!kernel || !kernel->perf.report_ready)
		return;
	printf("%s", kernel->perf.report_text);
	kernel->perf.report_ready = 0U;
}
#endif
void *scheduler_core1_app_stack_top(uint32_t slot) __attribute__((weak));
void *scheduler_core1_app_stack_top(uint32_t slot)
{
	(void)slot;
	return NULL;
}
void __attribute__((weak)) scheduler_core1_service_from_app(void)
{
}

void __attribute__((weak)) zzapp_kernel_switch_address_space(uint32_t slot)
{
	(void)slot;
}
void __attribute__((weak)) zzapp_kernel_switch_kernel_address_space(void)
{
}


#define NO_ACTIVE (ZZAPP_KERNEL_APP_SLOTS + 1U)

static int app_runnable(const zzapp_kernel_t *kernel)
{
	uint32_t slot;

	for (slot = 0; slot < ZZAPP_KERNEL_APP_SLOTS; slot++)
		if (kernel->threads[slot].state == ZZAPP_THREAD_RUNNABLE)
			return 1;
	return 0;
}
int zzapp_kernel_apps_runnable(const zzapp_kernel_t *kernel)
{
	return kernel && app_runnable(kernel);
}


static void wake_due(zzapp_kernel_t *kernel)
{
	for (;;) {
		uint32_t slot;
		uint32_t found = ZZAPP_KERNEL_APP_SLOTS;
		uint64_t deadline = UINT64_MAX;

		for (slot = 0; slot < ZZAPP_KERNEL_APP_SLOTS; slot++) {
			const zzapp_thread_t *thread = &kernel->threads[slot];
			if (thread->state == ZZAPP_THREAD_SLEEPING &&
			    thread->sleep_deadline <= kernel->now &&
			    (thread->sleep_deadline < deadline ||
			     (thread->sleep_deadline == deadline &&
			      (found == ZZAPP_KERNEL_APP_SLOTS ||
			       thread->wake_sequence <
			       kernel->threads[found].wake_sequence)))) {
				found = slot;
				deadline = thread->sleep_deadline;
			}
		}
		if (found == ZZAPP_KERNEL_APP_SLOTS)
			return;
		(void)zzapp_kernel_wake(kernel, found, ZZAPP_WAKE_TIMEOUT);
	}
}

void zzapp_kernel_init(zzapp_kernel_t *kernel)
{
	if (!kernel)
		return;
	memset(kernel, 0, sizeof(*kernel));
	kernel->active = NO_ACTIVE;
	kernel->api_lock_owner = -1;
	kernel->gl_owner = -1;
}

int zzapp_kernel_start_app(zzapp_kernel_t *kernel, uint32_t slot)
{
	if (!kernel || slot >= ZZAPP_KERNEL_APP_SLOTS)
		return -1;
	kernel->threads[slot].state = ZZAPP_THREAD_RUNNABLE;
	kernel->threads[slot].wake_reason = ZZAPP_WAKE_READY;
	kernel->threads[slot].quantum_left = ZZAPP_KERNEL_QUANTUM_MS;
	return 0;
}
int zzapp_kernel_prepare_app(zzapp_kernel_t *kernel, uint32_t slot,
			      uint32_t entry, uint32_t api, uint32_t argc,
			      uint32_t argv, uint32_t stack_top)
{
	zzapp_thread_context_t *context;

	if (!kernel || slot >= ZZAPP_KERNEL_APP_SLOTS || !entry || !stack_top)
		return -1;
#if ZZAPP_PERF_COUNTERS
	kernel->perf.audio_low_water[slot] = 0U;
	kernel->perf.audio_starved[slot] = 0U;
	kernel->perf.audio_underruns[slot] = 0U;
	kernel->perf.tlb_instruction_refills[slot] = 0U;
	kernel->perf.tlb_data_refills[slot] = 0U;
	if (kernel->perf.tlb_slot == slot)
		kernel->perf.tlb_slot = ZZAPP_KERNEL_APP_SLOTS;
#endif
	context = &kernel->threads[slot].context;
	memset(context, 0, sizeof(*context));
	context->r[0] = api;
	context->r[1] = argc;
	context->r[2] = argv;
	context->r[13] = stack_top;
	context->r[15] = entry;
	context->cpsr = ZZAPP_KERNEL_APP_CPSR;
	context->frame.user_sp = stack_top;
	context->frame.spsr = ZZAPP_KERNEL_APP_CPSR;
	context->frame.r[0] = api;
	context->frame.r[1] = argc;
	context->frame.r[2] = argv;
	context->frame.exception_lr = entry;
	return 0;
}


void zzapp_kernel_set_current_thread(int slot)
{
	current_thread = slot;
#if ZZAPP_PERF_COUNTERS
	zzapp_svc_counter_slot =
		slot >= 0 && slot < (int)ZZAPP_KERNEL_APP_SLOTS ? (uint32_t)slot : 0U;
#endif
	if (slot < 0 || slot >= (int)ZZAPP_KERNEL_APP_SLOTS)
		zzapp_kernel_svc_stack_top = 0U;
}

int zzapp_kernel_current_thread(void)
{
	return current_thread;
}

int zzapp_kernel_block(zzapp_kernel_t *kernel, uint32_t slot)
{
	if (!kernel || slot >= ZZAPP_KERNEL_APP_SLOTS)
		return -1;
	kernel->threads[slot].state = ZZAPP_THREAD_BLOCKED;
	kernel->threads[slot].quantum_left = 0;
	if (kernel->active == slot)
		kernel->active = NO_ACTIVE;
	return 0;
}

int zzapp_kernel_wake(zzapp_kernel_t *kernel, uint32_t slot,
		zzapp_wake_reason_t reason)
{
	zzapp_thread_t *thread;

	if (!kernel || slot >= ZZAPP_KERNEL_APP_SLOTS)
		return -1;
	thread = &kernel->threads[slot];
	if (thread->state != ZZAPP_THREAD_BLOCKED &&
	    thread->state != ZZAPP_THREAD_SLEEPING)
		return -1;
	thread->state = ZZAPP_THREAD_RUNNABLE;
	thread->wake_reason = reason;
	thread->quantum_left = ZZAPP_KERNEL_QUANTUM_MS;
	thread->wake_sequence = ++kernel->wake_serial;
	if (kernel->wake_log_count < ZZAPP_KERNEL_APP_SLOTS)
		kernel->wake_log[kernel->wake_log_count++] = (uint8_t)slot;
	return 0;
}

int zzapp_kernel_sleep(zzapp_kernel_t *kernel, uint32_t slot,
		uint64_t deadline)
{
	if (!kernel || slot >= ZZAPP_KERNEL_APP_SLOTS)
		return -1;
	kernel->threads[slot].state = ZZAPP_THREAD_SLEEPING;
	kernel->threads[slot].sleep_deadline = deadline;
	kernel->threads[slot].wake_sequence = ++kernel->wake_serial;
	kernel->threads[slot].quantum_left = 0;
	if (kernel->active == slot)
		kernel->active = NO_ACTIVE;
	return 0;
}

void zzapp_kernel_advance(zzapp_kernel_t *kernel, uint32_t elapsed_ms)
{
	if (!kernel)
		return;
	kernel->now += elapsed_ms;
	wake_due(kernel);
}

void zzapp_kernel_service_due(zzapp_kernel_t *kernel)
{
	if (kernel)
		kernel->service_due = 1;
}

static int choose_app(zzapp_kernel_t *kernel)
{
	uint32_t offset;

	if (kernel->active < ZZAPP_KERNEL_APP_SLOTS &&
	    kernel->threads[kernel->active].state == ZZAPP_THREAD_RUNNABLE &&
	    kernel->threads[kernel->active].quantum_left != 0U)
		return (int)kernel->active;
	for (offset = 0; offset < ZZAPP_KERNEL_APP_SLOTS; offset++) {
		uint32_t slot = (kernel->rr_next + offset) % ZZAPP_KERNEL_APP_SLOTS;
		if (kernel->threads[slot].state == ZZAPP_THREAD_RUNNABLE) {
			kernel->active = slot;
			kernel->rr_next = (slot + 1U) % ZZAPP_KERNEL_APP_SLOTS;
			if (kernel->threads[slot].quantum_left == 0U)
				kernel->threads[slot].quantum_left = ZZAPP_KERNEL_QUANTUM_MS;
			return (int)slot;
		}
	}
	kernel->active = NO_ACTIVE;
	return ZZAPP_KERNEL_IDLE;
}

/* Charge `slot` for a slice of up to `elapsed_ms`; an exhausted quantum
 * lets the next pick rotate to another runnable app. */
static void charge_slice(zzapp_kernel_t *kernel, uint32_t slot,
			 uint32_t elapsed_ms)
{
	zzapp_thread_t *thread = &kernel->threads[slot];
	uint32_t charged = elapsed_ms;

	if (charged > thread->quantum_left)
		charged = thread->quantum_left;
	thread->user_ticks += charged;
	thread->quantum_left -= charged;
	if (thread->quantum_left == 0U)
		kernel->active = NO_ACTIVE;
}

int zzapp_kernel_schedule(zzapp_kernel_t *kernel, uint32_t elapsed_ms)
{
	int pick;
	int runnable;

	if (!kernel)
		return ZZAPP_KERNEL_IDLE;
	zzapp_kernel_advance(kernel, elapsed_ms);
	if (kernel->now - kernel->window_start > ZZAPP_KERNEL_SERVICE_WINDOW_MS) {
		kernel->window_start = kernel->now;
		kernel->service_in_window = 0;
	}
	runnable = app_runnable(kernel);
	if (kernel->api_lock_owner == ZZAPP_KERNEL_SYSTEM_OWNER) {
		uint32_t slot;

		for (slot = 0U; slot < ZZAPP_KERNEL_APP_SLOTS; slot++) {
			if (!kernel->api_waiting[slot])
				continue;
			if (!runnable || kernel->service_in_window + elapsed_ms <=
			    ZZAPP_KERNEL_SERVICE_LIMIT_MS) {
				kernel->service_ticks += elapsed_ms;
				kernel->service_in_window += elapsed_ms;
				return ZZAPP_KERNEL_SYSTEM;
			}
			kernel->overload_count++;
			break;
		}
	} else if (kernel->api_lock_owner >= 0 &&
		   kernel->api_lock_owner < (int)ZZAPP_KERNEL_APP_SLOTS &&
		   kernel->threads[kernel->api_lock_owner].state ==
		   ZZAPP_THREAD_RUNNABLE) {
		pick = kernel->api_lock_owner;
		goto charge_app;
	}
	if (kernel->service_due &&
	    (!runnable || kernel->service_in_window + elapsed_ms <=
	     ZZAPP_KERNEL_SERVICE_LIMIT_MS)) {
		kernel->service_due = 0;
		kernel->service_ticks += elapsed_ms;
		kernel->service_in_window += elapsed_ms;
		return ZZAPP_KERNEL_SYSTEM;
	}
	if (kernel->service_due && runnable)
		kernel->overload_count++;
	pick = choose_app(kernel);
	if (pick >= 0) {
charge_app:
		charge_slice(kernel, (uint32_t)pick, elapsed_ms);
		return pick;
	}
	return ZZAPP_KERNEL_IDLE;
}
int zzapp_kernel_dispatch(zzapp_kernel_t *kernel,
			 zzapp_kernel_dispatch_t source, uint32_t elapsed_ms)
{
	if (!kernel)
		return ZZAPP_KERNEL_IDLE;
	if (source == ZZAPP_KERNEL_DISPATCH_DOORBELL)
		zzapp_kernel_service_due(kernel);
	else if (source == ZZAPP_KERNEL_DISPATCH_VOLUNTARY &&
		 kernel->active < ZZAPP_KERNEL_APP_SLOTS) {
		kernel->threads[kernel->active].quantum_left = 0U;
		kernel->active = NO_ACTIVE;
	}
	return zzapp_kernel_schedule(kernel, elapsed_ms);
}

int zzapp_kernel_wait_status(const zzapp_kernel_t *kernel, uint32_t slot)
{
	if (!kernel || slot >= ZZAPP_KERNEL_APP_SLOTS)
		return ZZAPP_KERNEL_WAIT_CANCELLED;
	if (kernel->threads[slot].wake_reason == ZZAPP_WAKE_TIMEOUT)
		return ZZAPP_KERNEL_WAIT_TIMEOUT;
	if (kernel->threads[slot].wake_reason == ZZAPP_WAKE_CANCELLED)
		return ZZAPP_KERNEL_WAIT_CANCELLED;
	return 0;
}
static void context_save(zzapp_thread_context_t *context,
			 const zzapp_arm_frame_t *frame, uint32_t adjust)
{
	memcpy(&context->frame, frame, sizeof(*frame));
	memcpy(context->d, frame->vfp, sizeof(frame->vfp));
	memcpy(context->r, frame->r, sizeof(frame->r));
	context->r[13] = frame->user_sp;
	context->r[14] = frame->user_lr;
	context->r[15] = frame->exception_lr - adjust;
	context->cpsr = frame->spsr;
	context->fpscr = frame->fpscr;
}

static zzapp_arm_frame_t *context_restore(zzapp_thread_context_t *context,
					   uint32_t adjust)
{
	zzapp_arm_frame_t *frame = &context->frame;

	frame->user_sp = context->r[13];
	frame->user_lr = context->r[14];
	frame->spsr = context->cpsr;
	memcpy(frame->vfp, context->d, sizeof(frame->vfp));
	frame->fpscr = context->fpscr;
	memcpy(frame->r, context->r, sizeof(frame->r));
	frame->exception_lr = context->r[15] + adjust;
	return frame;
}

zzapp_arm_frame_t *zzapp_kernel_switch_frame(zzapp_kernel_t *kernel,
					      zzapp_arm_frame_t *frame,
					      zzapp_kernel_dispatch_t source,
					      uint32_t return_adjust)
{
	int slot;
	int pick;

	if (!kernel || !frame)
		return frame;
#if ZZAPP_PERF_COUNTERS
	zzapp_kernel_perf_switch_finished(kernel);
#endif
	slot = zzapp_kernel_current_thread();
#if ZZAPP_PERF_COUNTERS
	kernel_perf_tlb_leave(kernel, slot);
#endif
	if (slot >= 0 && slot < (int)ZZAPP_KERNEL_APP_SLOTS) {
		context_save(&kernel->threads[slot].context, frame, return_adjust);
		pick = zzapp_kernel_dispatch(kernel, source,
					     source == ZZAPP_KERNEL_DISPATCH_TICK ?
					     ZZAPP_KERNEL_QUANTUM_MS : 0U);
	} else {
		context_save(&kernel->system_context, frame, return_adjust);
		kernel->system_context_valid = 1U;
		pick = zzapp_kernel_schedule(kernel,
			source == ZZAPP_KERNEL_DISPATCH_TICK ?
			ZZAPP_KERNEL_QUANTUM_MS : 0U);
		/* Ticks are charged to the app the scheduler picks for the next
		 * slice. Every tick also makes service due, so the tick usually
		 * goes to this thread, and the app it hands the core to after its
		 * pass would run uncharged: the same app kept its quantum forever
		 * and a second CPU-bound app got about 5% of the core (kit 66,
		 * two spin apps). Charge that slice too. */
		if (pick >= 0 && source != ZZAPP_KERNEL_DISPATCH_TICK)
			charge_slice(kernel, (uint32_t)pick, ZZAPP_KERNEL_QUANTUM_MS);
	}
	if ((pick == ZZAPP_KERNEL_SYSTEM || pick == ZZAPP_KERNEL_IDLE) &&
	    kernel->system_context_valid) {
		zzapp_kernel_set_current_thread(-1);
		zzapp_kernel_switch_kernel_address_space();
		return context_restore(&kernel->system_context, return_adjust);
	}
	if (pick < 0)
		return frame;
	zzapp_kernel_set_current_thread(pick);
	zzapp_kernel_svc_stack_top =
		(uintptr_t)scheduler_core1_app_stack_top((uint32_t)pick);
	zzapp_kernel_switch_address_space((uint32_t)pick);
#if ZZAPP_PERF_COUNTERS
	kernel_perf_tlb_enter(kernel, pick);
	zzapp_kernel_perf_request_report(kernel, (uint32_t)pick, 0);
#endif
	return context_restore(&kernel->threads[pick].context, return_adjust);
}



void zzapp_kernel_charge_svc(zzapp_kernel_t *kernel, uint32_t slot,
		uint32_t elapsed_ms)
{
	if (!kernel || slot >= ZZAPP_KERNEL_APP_SLOTS ||
	    kernel->threads[slot].state == ZZAPP_THREAD_BLOCKED)
		return;
	kernel->threads[slot].svc_ticks += elapsed_ms;
}

static void wake_api_waiters(zzapp_kernel_t *kernel)
{
	uint32_t slot;

	for (slot = 0U; slot < ZZAPP_KERNEL_APP_SLOTS; slot++) {
		if (!kernel->api_waiting[slot])
			continue;
		kernel->api_waiting[slot] = 0U;
		(void)zzapp_kernel_wake(kernel, slot, ZZAPP_WAKE_READY);
	}
}

static void release_api_lock(zzapp_kernel_t *kernel)
{
	kernel->api_lock_owner = -1;
	kernel->api_lock_depth = 0U;
	wake_api_waiters(kernel);
}

void zzapp_kernel_reset_lock(zzapp_kernel_t *kernel)
{
	if (!kernel)
		return;
	release_api_lock(kernel);
}

int zzapp_kernel_system_lock(zzapp_kernel_t *kernel)
{
	if (!kernel)
		return -1;
	if (kernel->api_lock_owner == ZZAPP_KERNEL_SYSTEM_OWNER) {
		kernel->api_lock_depth++;
		return 0;
	}
	if (kernel->api_lock_owner >= 0)
		return -1;
	kernel->api_lock_owner = ZZAPP_KERNEL_SYSTEM_OWNER;
	kernel->api_lock_depth = 1U;
	return 0;
}

int zzapp_kernel_system_unlock(zzapp_kernel_t *kernel)
{
	uint32_t irq_state;

	if (!kernel)
		return -1;
	irq_state = smp_local_irq_save();
	if (kernel->api_lock_owner != ZZAPP_KERNEL_SYSTEM_OWNER) {
		smp_local_irq_restore(irq_state);
		return -1;
	}
	if (--kernel->api_lock_depth == 0U)
		release_api_lock(kernel);
	smp_local_irq_restore(irq_state);
	return 0;
}

int zzapp_kernel_api_fault_release(zzapp_kernel_t *kernel, uint32_t slot)
{
	uint32_t irq_state;

	if (!kernel || slot >= ZZAPP_KERNEL_APP_SLOTS)
		return -1;
	irq_state = smp_local_irq_save();
	if (kernel->api_lock_owner != (int)slot) {
		smp_local_irq_restore(irq_state);
		return -1;
	}
	release_api_lock(kernel);
	smp_local_irq_restore(irq_state);
	return 0;
}

int zzapp_kernel_api_lock(zzapp_kernel_t *kernel, uint32_t slot)
{
	if (!kernel || slot >= ZZAPP_KERNEL_APP_SLOTS)
		return -1;
	if (kernel->api_lock_owner == (int)slot) {
		kernel->api_lock_depth++;
		return 0;
	}
	if (kernel->api_lock_owner >= 0 ||
	    kernel->api_lock_owner == ZZAPP_KERNEL_SYSTEM_OWNER) {
		kernel->api_waiting[slot] = 1U;
		return -1;
	}
	kernel->api_waiting[slot] = 0U;
	kernel->api_lock_owner = (int)slot;
	kernel->api_lock_depth = 1U;
	return 0;
}

int zzapp_kernel_api_unlock(zzapp_kernel_t *kernel, uint32_t slot)
{
	uint32_t irq_state;

	if (!kernel || slot >= ZZAPP_KERNEL_APP_SLOTS)
		return -1;
	irq_state = smp_local_irq_save();
	if (kernel->api_lock_owner != (int)slot) {
		smp_local_irq_restore(irq_state);
		return -1;
	}
	if (--kernel->api_lock_depth == 0U)
		release_api_lock(kernel);
	smp_local_irq_restore(irq_state);
	return 0;
}

int zzapp_kernel_api_block(zzapp_kernel_t *kernel, uint32_t slot)
{
	uint32_t irq_state;
	int rc;

	if (!kernel || slot >= ZZAPP_KERNEL_APP_SLOTS)
		return -1;
	irq_state = smp_local_irq_save();
	if (kernel->api_lock_owner == (int)slot)
		release_api_lock(kernel);
	rc = zzapp_kernel_block(kernel, slot);
	smp_local_irq_restore(irq_state);
	return rc;
}

static int surface_overlaps(uint32_t base_a, uint32_t bytes_a,
			    uint32_t base_b, uint32_t bytes_b)
{
	uint32_t end_a = base_a + bytes_a;
	uint32_t end_b = base_b + bytes_b;

	return end_a > base_b && end_b > base_a;
}

int zzapp_kernel_surface_bind(zzapp_kernel_t *kernel, uint32_t slot,
			      uint32_t generation, uint32_t base, uint32_t bytes)
{
	zzapp_kernel_resources_t *resource;
	uint32_t i;

	if (!kernel || slot >= ZZAPP_KERNEL_APP_SLOTS || !base || !bytes ||
	    (base & (ZZAPP_KERNEL_PAGE_BYTES - 1U)) != 0U ||
	    (bytes & (ZZAPP_KERNEL_PAGE_BYTES - 1U)) != 0U ||
	    base + bytes < base)
		return -1;
	resource = &kernel->resources[slot];
	if (resource->surface_bytes != 0U)
		return resource->surface_base == base &&
		       resource->surface_bytes == bytes &&
		       resource->surface_generation == generation ? 0 : -1;
	for (i = 0; i < ZZAPP_KERNEL_APP_SLOTS; i++) {
		const zzapp_kernel_resources_t *other = &kernel->resources[i];

		if (i != slot && other->surface_bytes != 0U &&
		    surface_overlaps(base, bytes, other->surface_base,
				     other->surface_bytes))
			return -1;
	}
	resource->surface_base = base;
	resource->surface_bytes = bytes;
	resource->surface_generation = generation;
	return 0;
}
void zzapp_kernel_surface_unbind(zzapp_kernel_t *kernel, uint32_t slot)
{
	if (!kernel || slot >= ZZAPP_KERNEL_APP_SLOTS)
		return;
	kernel->resources[slot].surface_base = 0U;
	kernel->resources[slot].surface_bytes = 0U;
	kernel->resources[slot].surface_generation = 0U;
}


int zzapp_kernel_audio_open(zzapp_kernel_t *kernel, uint32_t slot,
			    int direct_card_route)
{
	zzapp_kernel_resources_t *resource;

	if (!kernel || slot >= ZZAPP_KERNEL_APP_SLOTS)
		return -1;
	resource = &kernel->resources[slot];
	if (resource->direct_audio)
		return -1;
	if (direct_card_route && kernel->direct_audio_count >= 2U)
		return -1;
	resource->direct_audio = direct_card_route ? 1U : 2U;
	if (direct_card_route)
		kernel->direct_audio_count++;
	return 0;
}

void zzapp_kernel_audio_close(zzapp_kernel_t *kernel, uint32_t slot)
{
	zzapp_kernel_resources_t *resource;

	if (!kernel || slot >= ZZAPP_KERNEL_APP_SLOTS)
		return;
	resource = &kernel->resources[slot];
	if (resource->direct_audio == 1U && kernel->direct_audio_count != 0U)
		kernel->direct_audio_count--;
	resource->direct_audio = 0U;
}

int zzapp_kernel_gl_bind(zzapp_kernel_t *kernel, uint32_t slot,
			 uint32_t generation)
{
	if (!kernel || slot >= ZZAPP_KERNEL_APP_SLOTS)
		return -1;
	if (kernel->gl_owner >= 0)
		return kernel->gl_owner == (int)slot &&
		       kernel->gl_generation == generation ? 0 : -1;
	kernel->gl_owner = (int)slot;
	kernel->gl_generation = generation;
	kernel->resources[slot].gl_generation = generation;
	return 0;
}

void zzapp_kernel_gl_unbind(zzapp_kernel_t *kernel, uint32_t slot,
			    uint32_t generation)
{
	if (kernel && kernel->gl_owner == (int)slot &&
	    kernel->gl_generation == generation) {
		kernel->gl_owner = -1;
		kernel->gl_generation = 0U;
		kernel->resources[slot].gl_generation = 0U;
	}
}

int zzapp_kernel_session_open(zzapp_kernel_t *kernel, uint32_t slot,
			      int audio_session)
{
	uint32_t *sessions;

	if (!kernel || slot >= ZZAPP_KERNEL_APP_SLOTS)
		return -1;
	sessions = audio_session ? &kernel->resources[slot].audio_sessions :
				   &kernel->resources[slot].image_sessions;
	if (*sessions >= ZZAPP_KERNEL_SESSION_LIMIT)
		return -1;
	(*sessions)++;
	return 0;
}

void zzapp_kernel_session_close(zzapp_kernel_t *kernel, uint32_t slot,
			       int audio_session)
{
	uint32_t *sessions;

	if (!kernel || slot >= ZZAPP_KERNEL_APP_SLOTS)
		return;
	sessions = audio_session ? &kernel->resources[slot].audio_sessions :
				   &kernel->resources[slot].image_sessions;
	if (*sessions != 0U)
		(*sessions)--;
}

void zzapp_kernel_stop_app(zzapp_kernel_t *kernel, uint32_t slot)
{
	if (!kernel || slot >= ZZAPP_KERNEL_APP_SLOTS)
		return;
	zzapp_kernel_audio_close(kernel, slot);
	zzapp_kernel_gl_unbind(kernel, slot,
			      kernel->resources[slot].gl_generation);
	memset(&kernel->resources[slot], 0, sizeof(kernel->resources[slot]));
	kernel->threads[slot].wake_reason = ZZAPP_WAKE_CANCELLED;
	kernel->threads[slot].state = ZZAPP_THREAD_STOPPED;
	kernel->threads[slot].quantum_left = 0U;
	if (kernel->active == slot)
		kernel->active = NO_ACTIVE;
	if (kernel->api_lock_owner == (int)slot)
		release_api_lock(kernel);
	kernel->api_waiting[slot] = 0U;
}

void zzapp_kernel_stack_paint(uint8_t *stack, uint32_t bytes)
{
	if (stack)
		memset(stack, 0xa5, bytes);
}

uint32_t zzapp_kernel_stack_used(const uint8_t *stack, uint32_t bytes)
{
	uint32_t index = 0;

	if (!stack)
		return 0;
	while (index < bytes && stack[index] == 0xa5)
		index++;
	return bytes - index;
}
