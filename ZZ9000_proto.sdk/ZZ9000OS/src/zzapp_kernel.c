/*
 * Core-1 app scheduler policy.
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stddef.h>
#include <string.h>

#include "sdk_smp_lock.h"
#include "zzapp_kernel.h"
#include "zzapp_proto.h"
#include "zzapp_rec.h"

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
uint64_t __attribute__((weak)) zzapp_kernel_now_us(void)
{
	return 0U;
}
#endif

static volatile int current_thread = -1;
volatile uintptr_t zzapp_kernel_svc_stack_top;

/* The record area as the scheduler was given it. Kept apart from the kernel
 * struct for the one function without a kernel argument,
 * zzapp_kernel_set_current_thread(). */
static uint8_t *pub_threads;
static uint32_t pub_stride;
static uint32_t pub_capacity;

/* The thread is the first member of the record, so a thread pointer is a
 * record pointer. */
_Static_assert(offsetof(zzapp_rec_t, thread) == 0, "thread is the record base");
_Static_assert(ZZAPP_KERNEL_MAX_APPS == ZZAPP_REC_MAX_APPS,
	       "scheduler and handle table bounds agree");
#if ZZAPP_PERF_COUNTERS
_Static_assert(ZZAPP_REC_SVC_BYTES >= ZZAPP_SVC_COUNTER_BLOCK_BYTES,
	       "the svc part holds the counter block at its head");
#endif

/* The thread of a pid the caller has already checked against the capacity. */
static zzapp_thread_t *thread_at(const zzapp_kernel_t *kernel, uint32_t pid)
{
	return (zzapp_thread_t *)(void *)(kernel->threads +
					  (size_t)pid * kernel->stride);
}

static zzapp_thread_t *kernel_thread(const zzapp_kernel_t *kernel, uint32_t pid)
{
	if (!kernel || !kernel->threads || pid >= kernel->capacity)
		return NULL;
	return thread_at(kernel, pid);
}

static zzapp_rec_kernel_t *kernel_words(const zzapp_kernel_t *kernel,
					uint32_t pid)
{
	zzapp_rec_t *rec = (zzapp_rec_t *)(void *)kernel_thread(kernel, pid);

	return rec ? &rec->kernel : NULL;
}

/* The run list holds the pids whose thread is not STOPPED, ascending, so a
 * scan visits them in pid order whatever the capacity. It changes only when
 * a thread starts or stops, never from the timer IRQ, but the IRQ walks it:
 * mask local IRQs around every change. */
static void run_add(zzapp_kernel_t *kernel, uint32_t pid)
{
	zzapp_thread_t *thread = kernel_thread(kernel, pid);
	uint32_t irq_state = smp_local_irq_save();

	if (!thread->in_run) {
		uint32_t at = kernel->run_count;

		while (at > 0U && kernel->run[at - 1U] > pid) {
			kernel->run[at] = kernel->run[at - 1U];
			at--;
		}
		kernel->run[at] = (uint16_t)pid;
		kernel->run_count++;
		thread->in_run = 1U;
	}
	smp_local_irq_restore(irq_state);
}

static void run_remove(zzapp_kernel_t *kernel, uint32_t pid)
{
	zzapp_thread_t *thread = kernel_thread(kernel, pid);
	uint32_t irq_state = smp_local_irq_save();

	if (thread->in_run) {
		uint32_t at = 0U;

		while (at < kernel->run_count && kernel->run[at] != pid)
			at++;
		for (; at + 1U < kernel->run_count; at++)
			kernel->run[at] = kernel->run[at + 1U];
		if (kernel->run_count)
			kernel->run_count--;
		thread->in_run = 0U;
	}
	smp_local_irq_restore(irq_state);
}

#if ZZAPP_PERF_COUNTERS
/* The SVC vector counts through this pointer (see zzapp_kernel.h). */
void *volatile zzapp_svc_counter_block;
/* What it points at while no app thread runs. */
static uint32_t idle_counter_block[ZZAPP_SVC_COUNTER_BLOCK_BYTES / 4U]
	__attribute__((aligned(8)));
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

/*
 * TLB refill sampling runs only inside short windows opened when a report
 * is requested.  Reading both selectable PMU counters at every dispatch and
 * undispatch - four counter reads, four selections and their ISBs per
 * switch - cost measurable frame time on every one of the ~268k switches of
 * a swquake run, so between windows the PMU is not touched at all.
 *
 * Inside a window there is one sample per switch, taken where the record's
 * thread is undispatched: both counters are read with one selection change
 * each, and the delta from the previous sample is charged to the record that
 * held the core in between.  Kernel gaps between two samples of the same app are
 * charged to that app instead of being dropped; the alternative - sampling
 * at both edges - doubles the PMU traffic.
 */
static int kernel_perf_tlb_sampling(const zzapp_kernel_t *kernel)
{
	return kernel->now < kernel->perf.tlb_sample_until_ms;
}

static void kernel_perf_tlb_leave(zzapp_kernel_t *kernel, int pid)
{
	zzapp_kernel_perf_t *perf;
	zzapp_rec_kernel_t *words;
	uint32_t instruction;
	uint32_t data;
	uint32_t irq_state;

	if (!kernel)
		return;
	irq_state = smp_local_irq_save();
	perf = &kernel->perf;
	if (pid < 0 || (uint32_t)pid >= kernel->capacity) {
		if (!kernel_perf_tlb_sampling(kernel)) {
			perf->tlb_pid = ZZAPP_PID_NONE;
			perf->tlb_stamps_valid = 0U;
		}
		smp_local_irq_restore(irq_state);
		return;
	}
	if (perf->tlb_pid != (uint32_t)pid ||
	    !kernel_perf_tlb_sampling(kernel)) {
		perf->tlb_pid = ZZAPP_PID_NONE;
		perf->tlb_stamps_valid = 0U;
		smp_local_irq_restore(irq_state);
		return;
	}
#ifdef ZZAPP_HOST_TEST
	instruction = perf->tlb_instruction_stamp + 1U;
	data = perf->tlb_data_stamp + 1U;
#else
	__asm__ __volatile__("mcr p15, 0, %0, c9, c12, 5" : : "r"(0U));
	__asm__ __volatile__("isb" ::: "memory");
	__asm__ __volatile__("mrc p15, 0, %0, c9, c13, 2" : "=r"(instruction));
	__asm__ __volatile__("mcr p15, 0, %0, c9, c12, 5" : : "r"(1U));
	__asm__ __volatile__("isb" ::: "memory");
	__asm__ __volatile__("mrc p15, 0, %0, c9, c13, 2" : "=r"(data));
#endif
	words = kernel_words(kernel, (uint32_t)pid);
	if (perf->tlb_stamps_valid) {
		words->tlb_instruction_refills += instruction -
			perf->tlb_instruction_stamp;
		words->tlb_data_refills += data - perf->tlb_data_stamp;
		perf->total.tlb_instruction_refills += instruction -
			perf->tlb_instruction_stamp;
		perf->total.tlb_data_refills += data - perf->tlb_data_stamp;
	}
	perf->tlb_instruction_stamp = instruction;
	perf->tlb_data_stamp = data;
	perf->tlb_stamps_valid = 1U;
	perf->tlb_pid = ZZAPP_PID_NONE;
	smp_local_irq_restore(irq_state);
}

static void kernel_perf_tlb_enter(zzapp_kernel_t *kernel, int pid)
{
	if (pid < 0 || (uint32_t)pid >= kernel->capacity)
		return;
	/* No PMU access here: the next leave folds the interval into this
	 * record from the previous sample, so entering is two plain stores. */
	kernel->perf.tlb_pid = (uint32_t)pid;
}

static void kernel_perf_tlb_window_open(zzapp_kernel_t *kernel)
{
	if (kernel->now + 1000U > kernel->perf.tlb_sample_until_ms)
		kernel->perf.tlb_sample_until_ms = kernel->now + 1000U;
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

void zzapp_kernel_perf_work_published(zzapp_kernel_t *kernel, uint32_t count)
{
	if (kernel)
		kernel->perf.total.work_publications += count;
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

static zzapp_perf_launcher_step_t kernel_perf_launcher_step(uint32_t detail)
{
	switch (detail) {
	case 0x101U:
		return ZZAPP_PERF_LAUNCHER_ZERO;
	case 0x102U:
		return ZZAPP_PERF_LAUNCHER_COPY;
	case 0x103U:
		return ZZAPP_PERF_LAUNCHER_MAP;
	case 0x104U:
		return ZZAPP_PERF_LAUNCHER_RELOC;
	case 0x105U:
		return ZZAPP_PERF_LAUNCHER_APPLY;
	case 0x106U:
		return ZZAPP_PERF_LAUNCHER_ALLOC;
	default:
		return ZZAPP_PERF_LAUNCHER_OTHER;
	}
}

void zzapp_kernel_perf_service_end_detail(zzapp_kernel_t *kernel,
					   zzapp_perf_service_t service,
					   uint32_t stamp, uint32_t detail,
					   uint32_t bytes)
{
	zzapp_kernel_perf_metric_t *metric;
	uint32_t elapsed;
	uint32_t old_max;

	if (!kernel || service >= ZZAPP_PERF_SERVICE_COUNT)
		return;
	metric = &kernel->perf.total.service[service];
	elapsed = stamp - kernel->perf.service_start[service];
	old_max = metric->max;
	kernel_perf_add(metric, elapsed);
	if (elapsed > old_max) {
		kernel->perf.total.service_max_detail[service] = detail;
		kernel->perf.total.service_max_bytes[service] = bytes;
	}
	if (service == ZZAPP_PERF_SERVICE_LAUNCHER) {
		zzapp_perf_launcher_step_t step =
			kernel_perf_launcher_step(detail);

		if (elapsed > kernel->perf.total.launcher_step_max[step])
			kernel->perf.total.launcher_step_max[step] = elapsed;
	}
}

void zzapp_kernel_perf_service_end(zzapp_kernel_t *kernel,
				    zzapp_perf_service_t service,
				    uint32_t stamp)
{
	zzapp_kernel_perf_service_end_detail(kernel, service, stamp, 0U, 0U);
}

void zzapp_kernel_perf_audio(zzapp_kernel_t *kernel, uint32_t pid,
			      uint32_t low_water, uint32_t starved,
			      uint32_t underruns)
{
	zzapp_rec_kernel_t *words = kernel_words(kernel, pid);

	if (!words)
		return;
	words->audio_low_water += low_water;
	words->audio_starved += starved;
	words->audio_underruns += underruns;
}

void zzapp_kernel_perf_snapshot(const zzapp_kernel_t *kernel, uint32_t pid,
				 zzapp_kernel_perf_snapshot_t *snapshot)
{
	const zzapp_rec_kernel_t *words = kernel_words(kernel, pid);

	if (!words || !snapshot)
		return;
	*snapshot = kernel->perf.total;
	snapshot->audio_low_water = words->audio_low_water;
	snapshot->audio_starved = words->audio_starved;
	snapshot->audio_underruns = words->audio_underruns;
	snapshot->tlb_instruction_refills = words->tlb_instruction_refills;
	snapshot->tlb_data_refills = words->tlb_data_refills;
}

void zzapp_kernel_perf_finish_slot(zzapp_kernel_t *kernel, uint32_t pid)
{
	if (!kernel_thread(kernel, pid))
		return;
	kernel_perf_tlb_leave(kernel, (int)pid);
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

void zzapp_kernel_perf_request_report(zzapp_kernel_t *kernel, uint32_t pid,
				       int force)
{
	zzapp_rec_kernel_t *words = kernel_words(kernel, pid);

	if (!words)
		return;
	if (!force && kernel->perf.report_count &&
	    (uint32_t)kernel->now - kernel->perf.last_report_ms < 10000U)
		return;
	if (!kernel->perf.report_pending)
		kernel_perf_tlb_window_open(kernel);
	if (!words->perf_report_pending) {
		words->perf_report_pending = 1U;
		kernel->perf.report_pending++;
	}
	if (force && !words->perf_report_force) {
		words->perf_report_force = 1U;
		kernel->perf.report_force++;
	}
}

static const char *launcher_detail_name(uint32_t detail)
{
	switch (detail) {
	case 0x101U:
		return "loader-zero";
	case 0x102U:
		return "loader-copy";
	case 0x103U:
		return "loader-map";
	case 0x104U:
		return "loader-reloc";
	case 0x105U:
		return "loader-apply";
	case 0x106U:
		return "loader-alloc";
	default:
		return "worker";
}
}

int zzapp_kernel_perf_emit_reports(zzapp_kernel_t *kernel)
{
	static const char *const service_names[ZZAPP_PERF_SERVICE_COUNT] = {
		"sdk_span", "minigl_span", "launcher_span", "audio_span",
		"svc_span"
	};
	uint32_t pid;
	uint32_t forced;
	uint32_t i;
	uint32_t service;
	zzapp_rec_kernel_t *words;
	zzapp_kernel_perf_snapshot_t snapshot = { 0 };
	int offset;

	if (!kernel || kernel->perf.report_ready)
		return 0;
	if (!kernel->perf.report_pending)
		return 0;
	pid = ZZAPP_PID_NONE;
	/* A forced request is made as an app stops, so its record is already
	 * off the run list: look at every record, but only while a report is
	 * pending at all. */
	if (kernel->perf.report_force) {
		for (i = 0U; i < kernel->capacity; i++)
			if (kernel_words(kernel, i)->perf_report_force) {
				pid = i;
				break;
			}
		if (pid == ZZAPP_PID_NONE)
			kernel->perf.report_force = 0U;
	}
	if (pid == ZZAPP_PID_NONE) {
		if (kernel->perf.report_count &&
		    (uint32_t)kernel->now - kernel->perf.last_report_ms < 10000U)
			return 0;
		for (i = 0U; i < kernel->capacity; i++)
			if (kernel_words(kernel, i)->perf_report_pending) {
				pid = i;
				break;
			}
	}
	if (pid == ZZAPP_PID_NONE) {
		/* The count outlived the record it counted (cleared at its next
		 * admission). Nothing to report. */
		kernel->perf.report_pending = 0U;
		return 0;
	}
	words = kernel_words(kernel, pid);
	forced = words->perf_report_force;
	zzapp_kernel_perf_snapshot(kernel, pid, &snapshot);
	words->perf_report_pending = 0U;
	if (kernel->perf.report_pending)
		kernel->perf.report_pending--;
	if (forced) {
		words->perf_report_force = 0U;
		if (kernel->perf.report_force)
			kernel->perf.report_force--;
	} else
		kernel->perf.last_report_ms = (uint32_t)kernel->now;
	kernel->perf.report_count++;
	/* tlb below is window-sampled: the PMU counters are read only in
	 * the one-second windows each report request opens (see
	 * kernel_perf_tlb_leave), so the counts cover those windows, not
	 * the whole run. */
	offset = snprintf(kernel->perf.report_text, sizeof(kernel->perf.report_text),
			  "[app] perf slot=%lu forced=%u switch=%lu/%lu/%lu "
			  "sgi=%lu/%lu/%lu sdk=%lu/%lu/%lu sgi_burst=%lu/%lu "
			  "audio=%lu/%lu/%lu tlb=%lu/%lu",
			  (unsigned long)pid, forced != 0U,
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
			  (unsigned long)snapshot.work_publications,
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
	if (offset > 0 && (size_t)offset < sizeof(kernel->perf.report_text))
		offset += snprintf(kernel->perf.report_text + offset,
				   sizeof(kernel->perf.report_text) - (size_t)offset,
				   " sdk_max_op=0x%04lx launcher_max_step=%s:%lu",
				   (unsigned long)snapshot.service_max_detail[
					   ZZAPP_PERF_SERVICE_SDK],
				   launcher_detail_name(snapshot.service_max_detail[
					   ZZAPP_PERF_SERVICE_LAUNCHER]),
				   (unsigned long)snapshot.service_max_bytes[
					   ZZAPP_PERF_SERVICE_LAUNCHER]);
	if (offset > 0 && (size_t)offset < sizeof(kernel->perf.report_text))
		offset += snprintf(kernel->perf.report_text + offset,
				   sizeof(kernel->perf.report_text) - (size_t)offset,
				   " launcher_steps=zero:%lu/copy:%lu/map:%lu/"
				   "reloc:%lu/apply:%lu/alloc:%lu/other:%lu",
				   (unsigned long)snapshot.launcher_step_max[
					   ZZAPP_PERF_LAUNCHER_ZERO],
				   (unsigned long)snapshot.launcher_step_max[
					   ZZAPP_PERF_LAUNCHER_COPY],
				   (unsigned long)snapshot.launcher_step_max[
					   ZZAPP_PERF_LAUNCHER_MAP],
				   (unsigned long)snapshot.launcher_step_max[
					   ZZAPP_PERF_LAUNCHER_RELOC],
				   (unsigned long)snapshot.launcher_step_max[
					   ZZAPP_PERF_LAUNCHER_APPLY],
				   (unsigned long)snapshot.launcher_step_max[
					   ZZAPP_PERF_LAUNCHER_ALLOC],
				   (unsigned long)snapshot.launcher_step_max[
					   ZZAPP_PERF_LAUNCHER_OTHER]);
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
void __attribute__((weak)) scheduler_core1_service_from_app(void)
{
}

void __attribute__((weak)) zzapp_kernel_switch_address_space(uint32_t pid)
{
	(void)pid;
}
void __attribute__((weak)) zzapp_kernel_switch_kernel_address_space(void)
{
}


static int app_runnable(const zzapp_kernel_t *kernel)
{
	uint32_t i;

	for (i = 0U; i < kernel->run_count; i++)
		if (kernel_thread(kernel, kernel->run[i])->state ==
		    ZZAPP_THREAD_RUNNABLE)
			return 1;
	return 0;
}
int zzapp_kernel_apps_runnable(const zzapp_kernel_t *kernel)
{
	return kernel && app_runnable(kernel);
}


static void handoff_api_lock(zzapp_kernel_t *kernel);
static void register_api_waiter(zzapp_kernel_t *kernel, uint32_t pid);

/* A sleeper is due when the millisecond clock reached its deadline or, for a
 * sleep with a microsecond deadline (aar_sleep_us), the microsecond clock did.
 * The clock is read at most once per scan, and only when such a sleeper is
 * there to compare. */
static int sleeper_due(const zzapp_kernel_t *kernel, const zzapp_thread_t *thread,
		       uint64_t *now_us, int *have_now)
{
	if (thread->state != ZZAPP_THREAD_SLEEPING)
		return 0;
	if (thread->sleep_deadline <= kernel->now)
		return 1;
	if (thread->sleep_deadline_us == 0U)
		return 0;
	if (!*have_now) {
		*now_us = zzapp_kernel_now_us();
		*have_now = 1;
	}
	return thread->sleep_deadline_us <= *now_us;
}

/* Wake the earliest due sleeper, then the next, until none is due. Ties on
 * the deadline go to the one that went to sleep first. */
static void wake_due(zzapp_kernel_t *kernel)
{
	uint64_t now_us = 0U;
	int have_now = 0;

	for (;;) {
		uint32_t i;
		uint32_t found = ZZAPP_PID_NONE;
		uint64_t deadline = UINT64_MAX;

		for (i = 0U; i < kernel->run_count; i++) {
			uint32_t pid = kernel->run[i];
			const zzapp_thread_t *thread = kernel_thread(kernel, pid);

			if (sleeper_due(kernel, thread, &now_us, &have_now) &&
			    (thread->sleep_deadline < deadline ||
			     (thread->sleep_deadline == deadline &&
			      (found == ZZAPP_PID_NONE ||
			       thread->wake_sequence <
			       kernel_thread(kernel, found)->wake_sequence)))) {
				found = pid;
				deadline = thread->sleep_deadline;
			}
		}
		if (found == ZZAPP_PID_NONE)
			return;
		(void)zzapp_kernel_wake(kernel, found, ZZAPP_WAKE_TIMEOUT);
	}
}

/* The state of a scheduler over `capacity` records at `threads` (or over none,
 * for the system thread alone). Nothing is zero-valued by accident: the API
 * lock reads "free" and not "owned by record 0". */
static void kernel_attach(zzapp_kernel_t *kernel, void *threads,
			  uint32_t stride, uint32_t capacity)
{
	uint32_t pid;

	memset(kernel, 0, sizeof(*kernel));
	kernel->threads = threads;
	kernel->stride = stride;
	kernel->capacity = capacity;
	kernel->active = ZZAPP_PID_NONE;
	kernel->api_lock_owner = -1;
	kernel->api_handoff = ZZAPP_PID_NONE;
	kernel->yielder = ZZAPP_PID_NONE;
	kernel->gl_owner = -1;
#if ZZAPP_PERF_COUNTERS
	kernel->perf.tlb_pid = ZZAPP_PID_NONE;
	kernel->acct_last = ZZAPP_PID_NONE;
#endif
	/* The records outlive the scheduler: start every thread STOPPED with no
	 * stale kernel words. The contexts are rebuilt by prepare_app. */
	for (pid = 0U; pid < capacity; pid++) {
		memset(kernel_thread(kernel, pid), 0,
		       offsetof(zzapp_thread_t, context));
		memset(kernel_words(kernel, pid), 0, sizeof(zzapp_rec_kernel_t));
	}
	pub_threads = kernel->threads;
	pub_stride = stride;
	pub_capacity = capacity;
#if ZZAPP_PERF_COUNTERS
	zzapp_svc_counter_block = idle_counter_block;
#endif
}

int zzapp_kernel_init(zzapp_kernel_t *kernel, void *threads, uint32_t stride,
		      uint32_t capacity)
{
	if (!kernel || !threads || capacity == 0U ||
	    capacity > ZZAPP_KERNEL_MAX_APPS || stride < sizeof(zzapp_rec_t) ||
	    ((uintptr_t)threads & 7U) != 0U)
		return -1;
	kernel_attach(kernel, threads, stride, capacity);
	return 0;
}

void zzapp_kernel_init_system_only(zzapp_kernel_t *kernel)
{
	if (kernel)
		kernel_attach(kernel, NULL, 0U, 0U);
}

zzapp_thread_t *zzapp_kernel_thread(const zzapp_kernel_t *kernel, uint32_t pid)
{
	return kernel_thread(kernel, pid);
}

uint32_t zzapp_kernel_capacity(const zzapp_kernel_t *kernel)
{
	return kernel ? kernel->capacity : 0U;
}

uint32_t zzapp_kernel_run_count(const zzapp_kernel_t *kernel)
{
	return kernel ? kernel->run_count : 0U;
}

uint32_t zzapp_kernel_run_pid(const zzapp_kernel_t *kernel, uint32_t index)
{
	return kernel && index < kernel->run_count ? kernel->run[index] :
						     ZZAPP_PID_NONE;
}

uint32_t zzapp_kernel_stack_high_water(const zzapp_kernel_t *kernel,
				       uint32_t pid)
{
	const zzapp_thread_t *thread = kernel_thread(kernel, pid);
	const zzapp_rec_t *rec = (const zzapp_rec_t *)thread;

	if (!thread || rec->hdr.kstack_top <= rec->hdr.kstack_base)
		return 0U;
	return zzapp_kernel_stack_used(
		(const uint8_t *)(uintptr_t)rec->hdr.kstack_base,
		rec->hdr.kstack_top - rec->hdr.kstack_base);
}

/* The weight the scheduler divides by: a thread that was never given one
 * counts as 1. */
static uint32_t thread_weight(const zzapp_thread_t *thread)
{
	return thread->weight == 0U ? 1U : thread->weight;
}

/* Virtual run time of `us` microseconds of CPU at the thread's weight. */
static uint64_t vruntime_charge(const zzapp_thread_t *thread, uint64_t us)
{
	return us * ZZAPP_KERNEL_VRUNTIME_SCALE / thread_weight(thread);
}

/* The smallest virtual run time among the runnable threads; 0 when none is
 * runnable and `min` is untouched. */
static int runnable_vruntime_min(const zzapp_kernel_t *kernel, uint64_t *min)
{
	uint32_t i;
	int found = 0;

	for (i = 0U; i < kernel->run_count; i++) {
		const zzapp_thread_t *thread = kernel_thread(kernel,
							     kernel->run[i]);

		if (thread->state == ZZAPP_THREAD_RUNNABLE &&
		    (!found || thread->vruntime < *min)) {
			*min = thread->vruntime;
			found = 1;
		}
	}
	return found;
}

#if ZZAPP_PERF_COUNTERS
/* The scheduling counters (zzapp_sched_acct_t). Nothing here divides or
 * walks the run list: a thread's wait is the difference of two running totals
 * of the core's time, taken when the wait begins and ends. */
enum { ACCT_NONE = 0, ACCT_RUN, ACCT_WAIT, ACCT_PARK };

static uint64_t acct_slice_us(const zzapp_kernel_t *kernel, uint64_t now)
{
	return kernel->slice_valid && now > kernel->slice_started_us ?
	       now - kernel->slice_started_us : 0U;
}

/* Use the dispatch timestamp for all accounting inside a switch. A wake may
 * be followed by a pick in the same switch, before the new slice starts. */
static void acct_boundary_begin(zzapp_kernel_t *kernel, uint64_t now, int app)
{
	if (!kernel->idle) {
		uint64_t delta = acct_slice_us(kernel, now);

		if (app)
			kernel->acct_app_us += delta;
		else
			kernel->acct_sys_us += delta;
	}
	kernel->acct_at_boundary = 1U;
}

/* The clock and the core's totals at the same instant. Outside a dispatch,
 * include the part of the current slice that has already elapsed. */
static void acct_now(const zzapp_kernel_t *kernel, uint64_t *now,
		     uint64_t *sys, uint64_t *app)
{
	uint64_t delta;

	*sys = kernel->acct_sys_us;
	*app = kernel->acct_app_us;
	if (kernel->acct_at_boundary) {
		*now = kernel->slice_started_us;
		return;
	}
	*now = zzapp_kernel_now_us();
	if (kernel->idle)
		return;
	delta = acct_slice_us(kernel, *now);
	if ((uint32_t)current_thread < kernel->capacity)
		*app += delta;
	else
		*sys += delta;
}

static void acct_accumulate(zzapp_sched_acct_t *a, uint64_t now, uint64_t sys,
		       uint64_t app)
{
	if (a->state == ACCT_WAIT) {
		a->wait_sys_us += sys - a->snap_sys_us;
		a->wait_app_us += app - a->snap_app_us;
	} else if (a->state == ACCT_RUN) {
		a->run_us += now - a->since_us;
	} else if (a->state == ACCT_PARK) {
		a->park_us[a->park_kind] += now - a->since_us;
	}
}

static void acct_close(zzapp_sched_acct_t *a, uint64_t now, uint64_t sys,
		       uint64_t app)
{
	acct_accumulate(a, now, sys, app);
	a->state = ACCT_NONE;
}

static void acct_run(zzapp_sched_acct_t *a, uint64_t now)
{
	a->state = ACCT_RUN;
	a->since_us = now;
}

static void acct_wait(zzapp_sched_acct_t *a, uint64_t sys, uint64_t app)
{
	a->state = ACCT_WAIT;
	a->snap_sys_us = sys;
	a->snap_app_us = app;
}

static void acct_park(zzapp_sched_acct_t *a, uint32_t kind, uint64_t now)
{
	if (kind >= ZZAPP_KERNEL_PARK_KINDS)
		kind = 0U;
	a->state = ACCT_PARK;
	a->park_kind = (uint8_t)kind;
	a->since_us = now;
	a->park_n[kind]++;
}

/* The thread starts: its counters begin at zero, runnable behind the core's
 * holder. */
static void acct_start(const zzapp_kernel_t *kernel, zzapp_thread_t *thread)
{
	uint64_t now, sys, app;

	memset(&thread->acct, 0, sizeof(thread->acct));
	acct_now(kernel, &now, &sys, &app);
	acct_wait(&thread->acct, sys, app);
}

/* The thread parks for `kind`, or for what the services named. */
static void acct_block(const zzapp_kernel_t *kernel, zzapp_thread_t *thread,
		       uint32_t kind)
{
	zzapp_sched_acct_t *a = &thread->acct;
	uint64_t now, sys, app;

	acct_now(kernel, &now, &sys, &app);
	if (kind == 0U)
		kind = a->next_kind;
	a->next_kind = 0U;
	acct_close(a, now, sys, app);
	acct_park(a, kind, now);
}

static void acct_woken(const zzapp_kernel_t *kernel, zzapp_thread_t *thread,
		       uint32_t pid)
{
	zzapp_sched_acct_t *a = &thread->acct;
	uint64_t now, sys, app;

	acct_now(kernel, &now, &sys, &app);
	acct_close(a, now, sys, app);
	/* A wake before the app hands off the core ends its park without a
	 * wait behind itself. Inside a switch the pick decides who runs. */
	if (!kernel->acct_at_boundary && current_thread == (int)pid)
		acct_run(a, now);
	else
		acct_wait(a, sys, app);
	a->wakes++;
}

/* Stop at the live boundary, but do not sample again on repeated teardown. */
static void acct_stop(const zzapp_kernel_t *kernel, zzapp_thread_t *thread)
{
	uint64_t now, sys, app;

	if (thread->acct.state == ACCT_NONE)
		return;
	acct_now(kernel, &now, &sys, &app);
	acct_close(&thread->acct, now, sys, app);
}
#define ACCT_START(k, t) acct_start((k), (t))
#define ACCT_BLOCK(k, t, kind) acct_block((k), (t), (kind))
#define ACCT_WOKEN(k, t, pid) acct_woken((k), (t), (pid))
#define ACCT_STOP(k, t) acct_stop((k), (t))
#define ACCT_BOUNDARY_BEGIN(k, now, app) acct_boundary_begin((k), (now), (app))
#define ACCT_BOUNDARY_END(k) ((k)->acct_at_boundary = 0U)
#define PICK_REASON(k, r) ((k)->pick_reason = (r))
#define DEFER_NOTE(k) ((k)->defer_pending++)
#else
#define ACCT_START(k, t) ((void)0)
#define ACCT_BLOCK(k, t, kind) ((void)0)
#define ACCT_WOKEN(k, t, pid) ((void)0)
#define ACCT_STOP(k, t) ((void)0)
#define ACCT_BOUNDARY_BEGIN(k, now, app) ((void)0)
#define ACCT_BOUNDARY_END(k) ((void)0)
#define PICK_REASON(k, r) ((void)0)
#define DEFER_NOTE(k) ((void)0)
#endif

void zzapp_kernel_park_kind(zzapp_kernel_t *kernel, uint32_t pid,
			    uint32_t kind)
{
#if ZZAPP_PERF_COUNTERS
	zzapp_thread_t *thread = kernel_thread(kernel, pid);

	if (thread)
		thread->acct.next_kind = (uint8_t)(
			kind < ZZAPP_KERNEL_PARK_SLEEP ? kind : 0U);
#else
	(void)kernel;
	(void)pid;
	(void)kind;
#endif
}

#if ZZAPP_PERF_COUNTERS
int zzapp_kernel_sched_snapshot(const zzapp_kernel_t *kernel, uint32_t pid,
				zzapp_sched_acct_t *out)
{
	const zzapp_thread_t *thread = kernel_thread(kernel, pid);
	uint64_t now, sys, app;

	if (!thread || !out)
		return -1;
	*out = thread->acct;
	acct_now(kernel, &now, &sys, &app);
	acct_accumulate(out, now, sys, app);
	return 0;
}

void zzapp_kernel_sched_report(zzapp_kernel_t *kernel, uint32_t pid)
{
	static const char *const park_names[ZZAPP_KERNEL_PARK_KINDS] = {
		"lock", "file", "conwr", "conrd", "dispopen", "flip", "audio",
		"event", "sqspace", "audioreq", "sleep"
	};
	zzapp_sched_acct_t a;
	uint64_t weight;
	uint64_t clamp_us;
	uint32_t kind;

	if (zzapp_kernel_sched_snapshot(kernel, pid, &a) != 0)
		return;
	weight = thread_weight(thread_at(kernel, pid));
	clamp_us = (a.clamp_vr / ZZAPP_KERNEL_VRUNTIME_SCALE) * weight +
		   (a.clamp_vr % ZZAPP_KERNEL_VRUNTIME_SCALE) * weight /
		   ZZAPP_KERNEL_VRUNTIME_SCALE;
#define SCHED_MS(us) (unsigned long)((us) / 1000U), (unsigned long)((us) % 1000U)
	printf("[app] sched slot=%lu run=%lu.%03lu wait_sys=%lu.%03lu "
	       "wait_app=%lu.%03lu picks=%lu preempt=%lu/%lu/%lu/%lu wakes=%lu "
	       "clamps=%lu/%lu.%03lu defer=%lu",
	       (unsigned long)pid, SCHED_MS(a.run_us), SCHED_MS(a.wait_sys_us),
	       SCHED_MS(a.wait_app_us), (unsigned long)a.picks,
	       (unsigned long)a.preempt[ZZAPP_PREEMPT_SYSTEM],
	       (unsigned long)a.preempt[ZZAPP_PREEMPT_LAG],
	       (unsigned long)a.preempt[ZZAPP_PREEMPT_QUANTUM],
	       (unsigned long)a.preempt[ZZAPP_PREEMPT_OTHER],
	       (unsigned long)a.wakes, (unsigned long)a.clamps,
	       SCHED_MS(clamp_us), (unsigned long)a.deferrals);
	printf(" why=%lu/%lu/%lu/%lu/%lu yields=%lu/%lu/%lu hold=%lu",
	       (unsigned long)a.why[ZZAPP_PICK_STICKY],
	       (unsigned long)a.why[ZZAPP_PICK_LAG],
	       (unsigned long)a.why[ZZAPP_PICK_MIN],
	       (unsigned long)a.why[ZZAPP_PICK_OWNER],
	       (unsigned long)a.why[ZZAPP_PICK_HANDOFF],
	       (unsigned long)a.yields, (unsigned long)a.yields_given,
	       (unsigned long)a.yield_skips, (unsigned long)a.hold_skips);
	/* The weight the scheduler divides by and where the app ended, against
	 * the floor: two apps that share fairly end close together in virtual run
	 * time, whatever their weights. */
	{
		uint64_t vr_us = thread_at(kernel, pid)->vruntime /
				 ZZAPP_KERNEL_VRUNTIME_SCALE;
		uint64_t floor_us = kernel->vfloor / ZZAPP_KERNEL_VRUNTIME_SCALE;

		printf(" weight=%lu vr=%lu.%03lu floor=%lu.%03lu",
		       (unsigned long)weight, SCHED_MS(vr_us), SCHED_MS(floor_us));
	}
	for (kind = 0U; kind < ZZAPP_KERNEL_PARK_KINDS; kind++) {
		if (a.park_n[kind] == 0U)
			continue;
		printf(" park_%s=%lu.%03lu/%lu", park_names[kind],
		       SCHED_MS(a.park_us[kind]), (unsigned long)a.park_n[kind]);
	}
#undef SCHED_MS
	printf("\r\n");
}
#endif

void zzapp_kernel_reset_cpu_ticks(zzapp_kernel_t *kernel, uint32_t pid)
{
	zzapp_thread_t *thread = kernel_thread(kernel, pid);

	if (!thread)
		return;
	thread->user_ticks = 0U;
	thread->svc_ticks = 0U;
}

int zzapp_kernel_set_weight(zzapp_kernel_t *kernel, uint32_t pid,
			    uint32_t weight)
{
	zzapp_thread_t *thread = kernel_thread(kernel, pid);

	if (!thread || weight > ZZAPP_WEIGHT_MAX)
		return -1;
	thread->weight = weight;
	return 0;
}

int zzapp_kernel_start_app(zzapp_kernel_t *kernel, uint32_t pid)
{
	zzapp_thread_t *thread = kernel_thread(kernel, pid);
	uint64_t start;

	if (!thread)
		return -1;
	if (!runnable_vruntime_min(kernel, &start))
		start = kernel->vfloor;
	else if (start > kernel->vfloor)
		kernel->vfloor = start;
	thread->vruntime = start;
	thread->yield_pending = 0U;
	run_add(kernel, pid);
	thread->state = ZZAPP_THREAD_RUNNABLE;
	ACCT_START(kernel, thread);
	thread->wake_reason = ZZAPP_WAKE_READY;
	thread->quantum_left = ZZAPP_KERNEL_QUANTUM_MS;
	return 0;
}
int zzapp_kernel_prepare_app(zzapp_kernel_t *kernel, uint32_t pid,
			      uint32_t entry, uint32_t api, uint32_t argc,
			      uint32_t argv, uint32_t stack_top)
{
	zzapp_thread_t *thread = kernel_thread(kernel, pid);
	zzapp_thread_context_t *context;

	if (!thread || !entry || !stack_top)
		return -1;
#if ZZAPP_PERF_COUNTERS
	{
		zzapp_rec_kernel_t *words = kernel_words(kernel, pid);

		words->audio_low_water = 0U;
		words->audio_starved = 0U;
		words->audio_underruns = 0U;
		words->tlb_instruction_refills = 0U;
		words->tlb_data_refills = 0U;
	}
	if (kernel->perf.tlb_pid == pid)
		kernel->perf.tlb_pid = ZZAPP_PID_NONE;
#endif
#if !defined(ZZAPP_HOST_TEST) || defined(ZZAPP_KERNEL_PAINT_STACK)
	/* The record's stack is painted here, at START, so its high water mark
	 * counts this run only. The host build leaves it alone: its record
	 * addresses are 64-bit and do not fit the header fields. */
	{
		const zzapp_rec_t *rec = (const zzapp_rec_t *)thread;

		if (rec->hdr.kstack_top > rec->hdr.kstack_base)
			zzapp_kernel_stack_paint(
				(uint8_t *)(uintptr_t)rec->hdr.kstack_base,
				rec->hdr.kstack_top - rec->hdr.kstack_base);
	}
#endif
	context = &thread->context;
	memset(context, 0, sizeof(*context));
	context->frame.user_sp = stack_top;
	context->frame.spsr = ZZAPP_KERNEL_APP_CPSR;
	context->frame.r[0] = api;
	context->frame.r[1] = argc;
	context->frame.r[2] = argv;
	context->frame.exception_lr = entry;
	return 0;
}


void zzapp_kernel_set_current_thread(int pid)
{
	int app = pid >= 0 && (uint32_t)pid < pub_capacity;

	current_thread = pid;
#if ZZAPP_PERF_COUNTERS
	/* The SVC vector counts into whatever this points at; it must never be
	 * NULL, so a non-app thread gets the scratch block. */
	zzapp_svc_counter_block = app ?
		(void *)(pub_threads + (size_t)pid * pub_stride +
			 offsetof(zzapp_rec_t, svc)) :
		(void *)idle_counter_block;
#endif
	if (!app)
		zzapp_kernel_svc_stack_top = 0U;
}

int zzapp_kernel_current_thread(void)
{
	return current_thread;
}

int zzapp_kernel_block(zzapp_kernel_t *kernel, uint32_t pid)
{
	zzapp_thread_t *thread = kernel_thread(kernel, pid);

	if (!thread)
		return -1;
	run_add(kernel, pid);
	thread->state = ZZAPP_THREAD_BLOCKED;
	ACCT_BLOCK(kernel, thread, 0U);
	thread->quantum_left = 0;
	if (kernel->active == pid)
		kernel->active = ZZAPP_PID_NONE;
	return 0;
}

/* A wake can race the system worker's final runnable-app check. Leave an
 * event pending so a following WFE cannot hide that app until another tick. */
static void kernel_wake_event(void)
{
#ifdef ZZAPP_HOST_TEST
	extern void zzapp_test_kernel_wake_event(void) __attribute__((weak));

	if (zzapp_test_kernel_wake_event)
		zzapp_test_kernel_wake_event();
#else
	__asm__ __volatile__("sev" ::: "memory");
#endif
}

int zzapp_kernel_wake(zzapp_kernel_t *kernel, uint32_t pid,
		zzapp_wake_reason_t reason)
{
	zzapp_thread_t *thread = kernel_thread(kernel, pid);
	zzapp_rec_kernel_t *words = kernel_words(kernel, pid);

	if (!thread)
		return -1;
	if (thread->state != ZZAPP_THREAD_BLOCKED &&
	    thread->state != ZZAPP_THREAD_SLEEPING)
		return -1;
	/* A thread that was parked comes back no further behind than
	 * ZZAPP_KERNEL_WAKE_CREDIT below the floor, so it cannot bank the time
	 * it was away and then take the core from the others. */
	{
		uint64_t min;
		uint64_t credit = ZZAPP_KERNEL_WAKE_CREDIT;

		if (runnable_vruntime_min(kernel, &min) && min > kernel->vfloor)
			kernel->vfloor = min;
		if (thread->vruntime + credit < kernel->vfloor) {
#if ZZAPP_PERF_COUNTERS
			thread->acct.clamps++;
			thread->acct.clamp_vr += kernel->vfloor - credit -
						 thread->vruntime;
#endif
			thread->vruntime = kernel->vfloor - credit;
		}
	}
	thread->state = ZZAPP_THREAD_RUNNABLE;
	ACCT_WOKEN(kernel, thread, pid);
	thread->wake_reason = reason;
	thread->sleep_deadline_us = 0U;
	thread->quantum_left = ZZAPP_KERNEL_QUANTUM_MS;
	thread->wake_sequence = ++kernel->wake_serial;
	if (words->api_resume_waiting) {
		words->api_resume_waiting = 0U;
		register_api_waiter(kernel, pid);
		if (kernel->api_lock_owner == -1 &&
		    kernel->api_handoff == ZZAPP_PID_NONE)
			handoff_api_lock(kernel);
	}
	kernel->wake_log[kernel->wake_log_count % ZZAPP_KERNEL_WAKE_LOG] =
		(uint16_t)pid;
	kernel->wake_log_count++;
	kernel_wake_event();
	return 0;
}

int zzapp_kernel_sleep(zzapp_kernel_t *kernel, uint32_t pid,
		uint64_t deadline)
{
	zzapp_thread_t *thread = kernel_thread(kernel, pid);

	if (!thread)
		return -1;
	run_add(kernel, pid);
	thread->state = ZZAPP_THREAD_SLEEPING;
	ACCT_BLOCK(kernel, thread, ZZAPP_KERNEL_PARK_SLEEP);
	thread->sleep_deadline = deadline;
	thread->sleep_deadline_us = 0U;
	thread->wake_sequence = ++kernel->wake_serial;
	thread->quantum_left = 0;
	if (kernel->active == pid)
		kernel->active = ZZAPP_PID_NONE;
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

/* A runnable peer is entitled to the core a yield gives up when its virtual
 * run time is not above the yielder's by more than the wake granularity at
 * the peer's weight (the scale of the lag rule). The yielder's own value is
 * taken to now: the run since the last switch is not charged yet. */
static int yield_peer_entitled(const zzapp_kernel_t *kernel,
			       const zzapp_thread_t *yielder,
			       const zzapp_thread_t *peer)
{
	uint64_t own = yielder->vruntime;
	uint64_t now;
	uint64_t gap;

	if (kernel->slice_valid && !kernel->idle) {
		now = zzapp_kernel_now_us();
		if (now > kernel->slice_started_us)
			own += vruntime_charge(yielder, now - kernel->slice_started_us);
	}
	if (peer->vruntime <= own)
		return 1;
	gap = peer->vruntime - own;
	return gap <= ZZAPP_KERNEL_WAKE_GRAN &&
	       gap * thread_weight(peer) <= ZZAPP_KERNEL_WAKE_GRAN;
}

/* The yield in force is spent: the first pick that gives the core to an app
 * ends it, whoever held the core before, the system thread and the idle core
 * included. One pid records it, so no flag outlives a completed pick. */
static void yield_spent(zzapp_kernel_t *kernel)
{
	if (kernel->yielder < kernel->capacity)
		kernel_thread(kernel, kernel->yielder)->yield_pending = 0U;
	kernel->yielder = ZZAPP_PID_NONE;
}

/* The calling thread goes behind its runnable peers for the next pick, if a
 * peer is entitled to the core. With none the yield is nothing: the caller
 * keeps the core and its quantum, and no peer is handed a quantum it has not
 * earned. */
void zzapp_kernel_yield(zzapp_kernel_t *kernel)
{
	uint32_t i;
	int current;
	zzapp_thread_t *yielder;

	if (!kernel)
		return;
	current = zzapp_kernel_current_thread();
	if (current < 0 || (uint32_t)current >= kernel->capacity)
		return;
	yielder = kernel_thread(kernel, (uint32_t)current);
#if ZZAPP_PERF_COUNTERS
	yielder->acct.yields++;
#endif
	for (i = 0U; i < kernel->run_count; i++) {
		uint32_t pid = kernel->run[i];
		const zzapp_thread_t *peer = kernel_thread(kernel, pid);

		if (pid != (uint32_t)current && peer->state == ZZAPP_THREAD_RUNNABLE &&
		    yield_peer_entitled(kernel, yielder, peer)) {
			/* A yield not yet spent by a pick gives way to this one.
			 * The flag and its owner are published together: an IRQ
			 * between the two stores would let a pick see the flag
			 * while yield_spent() cannot find its owner, and the
			 * yielder would stay excluded from every pick. */
			uint32_t irq_state = smp_local_irq_save();

			yield_spent(kernel);
			yielder->yield_pending = 1U;
			kernel->yielder = (uint32_t)current;
			smp_local_irq_restore(irq_state);
#if ZZAPP_PERF_COUNTERS
			yielder->acct.yields_given++;
#endif
			return;
		}
	}
}


/* The run time since the interval began, in whole milliseconds of the clock
 * (the difference of the floored readings, so remainders are not lost
 * between intervals) and in microseconds. */
static void slice_elapsed(const zzapp_kernel_t *kernel, uint64_t now_us,
			  uint32_t *elapsed_ms, uint32_t *elapsed_us)
{
	uint64_t delta_ms = 0U;
	uint64_t delta_us = 0U;

	if (kernel->slice_valid && now_us >= kernel->slice_started_us) {
		delta_us = now_us - kernel->slice_started_us;
		delta_ms = now_us / 1000U - kernel->slice_started_us / 1000U;
	}
	*elapsed_ms = delta_ms > UINT32_MAX ? UINT32_MAX : (uint32_t)delta_ms;
	*elapsed_us = delta_us > UINT32_MAX ? UINT32_MAX : (uint32_t)delta_us;
}

uint64_t zzapp_kernel_now(const zzapp_kernel_t *kernel)
{
	uint64_t now;

	if (!kernel)
		return 0U;
	now = kernel->now;
	if (kernel->slice_valid && !kernel->idle) {
		uint32_t elapsed_ms;
		uint32_t elapsed_us;

		slice_elapsed(kernel, zzapp_kernel_now_us(), &elapsed_ms,
			      &elapsed_us);
		now += elapsed_ms;
	}
	return now;
}

void zzapp_kernel_reset_slice(zzapp_kernel_t *kernel)
{
	if (!kernel)
		return;
	kernel->slice_valid = 0U;
	kernel->slice_started_us = 0U;
	kernel->idle = 0U;
}

void zzapp_kernel_idle_begin(zzapp_kernel_t *kernel)
{
	uint32_t irq_state;
	uint64_t now_us;
	uint32_t elapsed;
	uint32_t elapsed_us;

	if (!kernel)
		return;
	irq_state = smp_local_irq_save();
	if (kernel->idle) {
		smp_local_irq_restore(irq_state);
		return;
	}
	now_us = zzapp_kernel_now_us();
	slice_elapsed(kernel, now_us, &elapsed, &elapsed_us);
	ACCT_BOUNDARY_BEGIN(kernel, now_us, 0);
	kernel->slice_started_us = now_us;
	kernel->slice_valid = 1U;
	zzapp_kernel_advance(kernel, elapsed);
	ACCT_BOUNDARY_END(kernel);
	if (kernel->now - kernel->window_start > ZZAPP_KERNEL_SERVICE_WINDOW_MS) {
		kernel->window_start = kernel->now;
		kernel->service_in_window = 0;
	}
	if (elapsed != 0U) {
		kernel->service_ticks += elapsed;
		kernel->service_in_window += elapsed;
	}
	kernel->idle = 1U;
	smp_local_irq_restore(irq_state);
}

void zzapp_kernel_idle_end(zzapp_kernel_t *kernel)
{
	uint32_t irq_state;
	uint64_t now_us;
	uint32_t elapsed;
	uint32_t elapsed_us;

	if (!kernel)
		return;
	irq_state = smp_local_irq_save();
	if (!kernel->idle) {
		smp_local_irq_restore(irq_state);
		return;
	}
	now_us = zzapp_kernel_now_us();
	slice_elapsed(kernel, now_us, &elapsed, &elapsed_us);
	ACCT_BOUNDARY_BEGIN(kernel, now_us, 0);
	kernel->slice_started_us = now_us;
	kernel->slice_valid = 1U;
	kernel->idle = 0U;
	kernel->idle_ticks += elapsed;
	zzapp_kernel_advance(kernel, elapsed);
	ACCT_BOUNDARY_END(kernel);
	if (kernel->now - kernel->window_start > ZZAPP_KERNEL_SERVICE_WINDOW_MS) {
		kernel->window_start = kernel->now;
		kernel->service_in_window = 0;
	}
	smp_local_irq_restore(irq_state);
}

/* A runnable thread lags `active` by more than the wake granularity of its
 * own run time: its value is below the active one's by more than
 * ZZAPP_KERNEL_WAKE_GRAN_US microseconds at its weight, which is
 * GRAN / weight in virtual run time. Compared as gap * weight > GRAN so the
 * pick needs no division; a gap above GRAN is behind at any weight, and
 * the product stays below 2^26 * 2^8 otherwise. Threads that yielded do not
 * count: they are passed over for the pick anyway. */
static int active_is_lagged(const zzapp_kernel_t *kernel,
			    const zzapp_thread_t *active)
{
	uint32_t i;

	for (i = 0U; i < kernel->run_count; i++) {
		const zzapp_thread_t *thread = kernel_thread(kernel, kernel->run[i]);
		uint64_t gap;

		if (thread == active || thread->state != ZZAPP_THREAD_RUNNABLE ||
		    thread->yield_pending || thread->vruntime >= active->vruntime)
			continue;
		gap = active->vruntime - thread->vruntime;
		if (gap > ZZAPP_KERNEL_WAKE_GRAN ||
		    gap * thread_weight(thread) > ZZAPP_KERNEL_WAKE_GRAN)
			return 1;
	}
#if ZZAPP_PERF_COUNTERS
	/* The active thread stays: the runnable threads behind it, by less than
	 * the granularity, were held off. */
	for (i = 0U; i < kernel->run_count; i++) {
		zzapp_thread_t *thread = thread_at(kernel, kernel->run[i]);

		if (thread != active && thread->state == ZZAPP_THREAD_RUNNABLE &&
		    !thread->yield_pending && thread->vruntime < active->vruntime)
			thread->acct.hold_skips++;
	}
#endif
	return 0;
}

/* The runnable thread with the smallest virtual run time. Threads of equal
 * virtual run time take turns: the scan starts at the first pid at or above
 * rr_next and wraps, and only a strictly smaller value displaces the
 * candidate. A thread that yielded is passed over for this pick unless it is
 * the only one runnable. The active thread keeps the core for the rest of its
 * quantum unless another runnable thread lags it by the wake granularity. */
static int choose_app(zzapp_kernel_t *kernel)
{
	uint32_t count = kernel->run_count;
	uint32_t start;
	uint32_t i;
	uint32_t pick = ZZAPP_PID_NONE;
	uint32_t yielded = ZZAPP_PID_NONE;
	uint64_t pick_vr = 0U;
	uint64_t yielded_vr = 0U;
	uint64_t lowest = 0U;
	int have_floor = 0;
	int lagged = 0;

	if (kernel->active < kernel->capacity &&
	    kernel_thread(kernel, kernel->active)->state == ZZAPP_THREAD_RUNNABLE &&
	    kernel_thread(kernel, kernel->active)->quantum_left != 0U &&
	    !kernel_thread(kernel, kernel->active)->yield_pending) {
		if (!active_is_lagged(kernel, kernel_thread(kernel, kernel->active))) {
			PICK_REASON(kernel, ZZAPP_PICK_STICKY);
			return (int)kernel->active;
		}
		lagged = 1;
	}
	for (start = 0U; start < count && kernel->run[start] < kernel->rr_next;
	     start++)
		;
	for (i = 0U; i < count; i++) {
		uint32_t pid = kernel->run[(start + i) % count];
		const zzapp_thread_t *thread = kernel_thread(kernel, pid);

		if (thread->state != ZZAPP_THREAD_RUNNABLE)
			continue;
		if (!have_floor || thread->vruntime < lowest) {
			lowest = thread->vruntime;
			have_floor = 1;
		}
		if (thread->yield_pending) {
			if (yielded == ZZAPP_PID_NONE || thread->vruntime < yielded_vr) {
				yielded = pid;
				yielded_vr = thread->vruntime;
			}
		} else if (pick == ZZAPP_PID_NONE || thread->vruntime < pick_vr) {
			pick = pid;
			pick_vr = thread->vruntime;
		}
	}
	if (have_floor && lowest > kernel->vfloor)
		kernel->vfloor = lowest;
#if ZZAPP_PERF_COUNTERS
	if (pick != ZZAPP_PID_NONE && yielded != ZZAPP_PID_NONE &&
	    yielded_vr < pick_vr)
		kernel_thread(kernel, yielded)->acct.yield_skips++;
#endif
	if (pick == ZZAPP_PID_NONE)
		pick = yielded;
	if (pick == ZZAPP_PID_NONE) {
		kernel->active = ZZAPP_PID_NONE;
		return ZZAPP_KERNEL_IDLE;
	}
	(void)lagged;
	PICK_REASON(kernel, lagged ? ZZAPP_PICK_LAG : ZZAPP_PICK_MIN);
	kernel->active = pick;
	kernel->rr_next = pick + 1U;
	if (kernel_thread(kernel, pick)->quantum_left == 0U)
		kernel_thread(kernel, pick)->quantum_left = ZZAPP_KERNEL_QUANTUM_MS;
	return (int)pick;
}

static void charge_vruntime(zzapp_thread_t *thread, uint64_t elapsed_us)
{
	thread->vruntime += vruntime_charge(thread, elapsed_us);
}

/* Charge `pid` for a slice of up to `elapsed_ms` (`elapsed_us` of virtual
 * run time); an exhausted quantum lets the next pick rotate to another
 * runnable app. Privileged time is the API body, recorded separately from
 * user mode. */
static void charge_slice(zzapp_kernel_t *kernel, uint32_t pid,
			 uint32_t elapsed_ms, uint64_t elapsed_us,
			 int privileged)
{
	zzapp_thread_t *thread = kernel_thread(kernel, pid);

	/* Accumulate the full elapsed interval that ran. An app that
	 * blocked or went to sleep had its quantum zeroed before the
	 * switch, but the interval it executed before blocking must
	 * not be lost. */
	if (privileged)
		thread->svc_ticks += elapsed_ms;
	else
		thread->user_ticks += elapsed_ms;
	charge_vruntime(thread, elapsed_us);

	/* Clamp only the quantum decrement. */
	if (thread->quantum_left <= elapsed_ms)
		thread->quantum_left = 0U;
	else
		thread->quantum_left -= elapsed_ms;

	if (thread->quantum_left == 0U)
		kernel->active = ZZAPP_PID_NONE;
}

int zzapp_kernel_schedule(zzapp_kernel_t *kernel, uint32_t elapsed_ms)
{
	int pick;
	int runnable;

	if (!kernel)
		return ZZAPP_KERNEL_IDLE;
	PICK_REASON(kernel, ZZAPP_PICK_NONE);
	zzapp_kernel_advance(kernel, elapsed_ms);
	if (kernel->now - kernel->window_start > ZZAPP_KERNEL_SERVICE_WINDOW_MS) {
		kernel->window_start = kernel->now;
		kernel->service_in_window = 0;
	}
	runnable = app_runnable(kernel);
	if (kernel->api_lock_owner == -1 &&
	    kernel->api_handoff != ZZAPP_PID_NONE &&
	    !kernel->api_handoff_service_deferred) {
		pick = (int)kernel->api_handoff;
		if (kernel_thread(kernel, kernel->api_handoff)->state ==
		    ZZAPP_THREAD_RUNNABLE) {
			PICK_REASON(kernel, ZZAPP_PICK_HANDOFF);
			goto charge_app;
		}
		kernel->api_handoff = ZZAPP_PID_NONE;
		kernel->api_handoff_service_deferred = 0U;
	}
	if (kernel->api_lock_owner == ZZAPP_KERNEL_SYSTEM_OWNER) {
		uint32_t i;

		for (i = 0U; i < kernel->run_count; i++) {
			if (!kernel_words(kernel, kernel->run[i])->api_waiting)
				continue;
			if (!runnable || kernel->service_in_window + elapsed_ms <=
			    ZZAPP_KERNEL_SERVICE_LIMIT_MS) {
				kernel->service_ticks += elapsed_ms;
				kernel->service_in_window += elapsed_ms;
				PICK_REASON(kernel, ZZAPP_PICK_SYSTEM);
				return ZZAPP_KERNEL_SYSTEM;
			}
			kernel->overload_count++;
			DEFER_NOTE(kernel);
			break;
		}
	} else if (kernel->api_lock_owner >= 0 &&
		   (uint32_t)kernel->api_lock_owner < kernel->capacity &&
		   kernel_thread(kernel, (uint32_t)kernel->api_lock_owner)->state ==
		   ZZAPP_THREAD_RUNNABLE) {
		pick = kernel->api_lock_owner;
		PICK_REASON(kernel, ZZAPP_PICK_OWNER);
		goto charge_app;
	}
	if (kernel->service_due &&
	    (!runnable || kernel->service_in_window + elapsed_ms <=
	     ZZAPP_KERNEL_SERVICE_LIMIT_MS)) {
		kernel->service_due = 0;
		kernel->service_ticks += elapsed_ms;
		kernel->service_in_window += elapsed_ms;
		PICK_REASON(kernel, ZZAPP_PICK_SYSTEM);
		return ZZAPP_KERNEL_SYSTEM;
	}
	if (kernel->service_due && runnable) {
		kernel->overload_count++;
		DEFER_NOTE(kernel);
	}
	if (kernel->api_lock_owner == -1 &&
	    kernel->api_handoff != ZZAPP_PID_NONE) {
		pick = (int)kernel->api_handoff;
		if (kernel_thread(kernel, kernel->api_handoff)->state ==
		    ZZAPP_THREAD_RUNNABLE) {
			PICK_REASON(kernel, ZZAPP_PICK_HANDOFF);
			goto charge_app;
		}
		kernel->api_handoff = ZZAPP_PID_NONE;
		kernel->api_handoff_service_deferred = 0U;
	}
	pick = choose_app(kernel);
	if (pick >= 0) {
charge_app:
		yield_spent(kernel);
		charge_slice(kernel, (uint32_t)pick, elapsed_ms,
			     (uint64_t)elapsed_ms * 1000U, 0);
		return pick;
	}
	return ZZAPP_KERNEL_IDLE;
}
int zzapp_kernel_dispatch(zzapp_kernel_t *kernel,
			 zzapp_kernel_dispatch_t source, uint32_t elapsed_ms)
{
	int pick;

	if (!kernel)
		return ZZAPP_KERNEL_IDLE;
	if (source == ZZAPP_KERNEL_DISPATCH_DOORBELL)
		zzapp_kernel_service_due(kernel);
	else if (source == ZZAPP_KERNEL_DISPATCH_VOLUNTARY) {
		if (kernel->active < kernel->capacity) {
			kernel_thread(kernel, kernel->active)->quantum_left = 0U;
			kernel->active = ZZAPP_PID_NONE;
		}
	}
	pick = zzapp_kernel_schedule(kernel, elapsed_ms);
	return pick;
}

int zzapp_kernel_wait_status(const zzapp_kernel_t *kernel, uint32_t pid)
{
	const zzapp_thread_t *thread = kernel_thread(kernel, pid);

	if (!thread)
		return ZZAPP_KERNEL_WAIT_CANCELLED;
	if (thread->wake_reason == ZZAPP_WAKE_TIMEOUT)
		return ZZAPP_KERNEL_WAIT_TIMEOUT;
	if (thread->wake_reason == ZZAPP_WAKE_CANCELLED)
		return ZZAPP_KERNEL_WAIT_CANCELLED;
	return 0;
}
static void context_save(zzapp_thread_context_t *context,
			 const zzapp_arm_frame_t *frame, uint32_t adjust)
{
	memcpy(&context->frame, frame, sizeof(*frame));
	context->frame.exception_lr -= adjust;
}

static zzapp_arm_frame_t *context_restore(const zzapp_thread_context_t *context,
					   zzapp_arm_frame_t *frame,
					   uint32_t adjust)
{
	memcpy(frame, &context->frame, sizeof(*frame));
	frame->exception_lr += adjust;
	return frame;
}

#if ZZAPP_PERF_COUNTERS
/* A switch was made: `cur` held the core, `pick` has it now. The totals are
 * as of the switch, the interval before it already charged. An app that
 * leaves the core while still runnable begins to wait, and preempt[SYSTEM]
 * counts it when the system thread takes the core. The app that held the core
 * last (acct_last, which the system thread's passes in between leave alone)
 * is preempted by the pick that gives the core to another app while it is
 * still runnable, counted by that pick's reason: a woken app that lags it
 * comes through the system thread, so this is where wake preemption shows.
 * An app that is given the core leaves its wait. */
static void acct_switch(zzapp_kernel_t *kernel, int cur, int app, int pick)
{
	uint64_t now = kernel->slice_started_us;
	uint64_t sys = kernel->acct_sys_us;
	uint64_t apps = kernel->acct_app_us;

	if (app && cur != pick) {
		zzapp_thread_t *thread = thread_at(kernel, (uint32_t)cur);

		if (thread->acct.state == ACCT_RUN) {
			acct_close(&thread->acct, now, sys, apps);
			if (thread->state == ZZAPP_THREAD_RUNNABLE) {
				if (pick < 0 && kernel->pick_reason == ZZAPP_PICK_SYSTEM)
					thread->acct.preempt[ZZAPP_PREEMPT_SYSTEM]++;
				acct_wait(&thread->acct, sys, apps);
				kernel->acct_last = (uint32_t)cur;
			}
		}
	}
	if (pick >= 0 && (uint32_t)pick < kernel->capacity) {
		zzapp_thread_t *thread = thread_at(kernel, (uint32_t)pick);

		if (kernel->acct_last < kernel->capacity &&
		    kernel->acct_last != (uint32_t)pick) {
			zzapp_thread_t *last = thread_at(kernel, kernel->acct_last);

			if (last->state == ZZAPP_THREAD_RUNNABLE &&
			    last->acct.state == ACCT_WAIT) {
				uint32_t kind;

				switch (kernel->pick_reason) {
				case ZZAPP_PICK_LAG:
					kind = ZZAPP_PREEMPT_LAG;
					break;
				case ZZAPP_PICK_MIN:
					kind = ZZAPP_PREEMPT_QUANTUM;
					break;
				default:
					kind = ZZAPP_PREEMPT_OTHER;
					break;
				}
				last->acct.preempt[kind]++;
			}
		}
		kernel->acct_last = (uint32_t)pick;
		if (thread->acct.state != ACCT_RUN) {
			acct_close(&thread->acct, now, sys, apps);
			acct_run(&thread->acct, now);
			thread->acct.picks++;
		}
		thread->acct.why[kernel->pick_reason]++;
		thread->acct.deferrals += kernel->defer_pending;
		kernel->defer_pending = 0U;
	}
}
#define ACCT_SWITCH(k, cur, app, pick) acct_switch((k), (cur), (app), (pick))
#else
#define ACCT_SWITCH(k, cur, app, pick) ((void)0)
#endif

zzapp_arm_frame_t *zzapp_kernel_switch_frame(zzapp_kernel_t *kernel,
					      zzapp_arm_frame_t *frame,
					      zzapp_kernel_dispatch_t source,
					      uint32_t return_adjust)
{
	int cur;
	int pick;
	int app;

	if (!kernel || !frame)
		return frame;
#if ZZAPP_PERF_COUNTERS
	zzapp_kernel_perf_switch_finished(kernel);
#endif
	cur = zzapp_kernel_current_thread();
	app = cur >= 0 && (uint32_t)cur < kernel->capacity;
#if ZZAPP_PERF_COUNTERS
	kernel_perf_tlb_leave(kernel, cur);
#endif
	/* The interval since this context was dispatched, not a fixed quantum
	 * and not zero on a voluntary handoff. */
	{
		uint64_t now_us = zzapp_kernel_now_us();
		uint32_t elapsed;
		uint32_t elapsed_us;
		int privileged = 0;
		int was_idle = kernel->idle;

		slice_elapsed(kernel, now_us, &elapsed, &elapsed_us);
		ACCT_BOUNDARY_BEGIN(kernel, now_us, app);
		kernel->slice_started_us = now_us;
		kernel->slice_valid = 1U;
		kernel->idle = 0U;
		if (was_idle)
			kernel->idle_ticks += elapsed;
		if (app && frame)
			privileged = (frame->spsr & 0x1fU) != ZZAPP_KERNEL_APP_CPSR;
		zzapp_kernel_advance(kernel, elapsed);
		if (kernel->now - kernel->window_start >
		    ZZAPP_KERNEL_SERVICE_WINDOW_MS) {
			kernel->window_start = kernel->now;
			kernel->service_in_window = 0;
		}
		if (!was_idle && (elapsed != 0U || elapsed_us != 0U)) {
			if (app && elapsed != 0U)
				charge_slice(kernel, (uint32_t)cur, elapsed,
					     elapsed_us, privileged);
			else if (app)
				/* Under a millisecond: no tick to count, but
				 * the time is the app's all the same. */
				charge_vruntime(thread_at(kernel, (uint32_t)cur),
						elapsed_us);
			else {
				kernel->service_ticks += elapsed;
				kernel->service_in_window += elapsed;
			}
		}
	}
	if (app) {
		context_save(&thread_at(kernel, (uint32_t)cur)->context, frame,
			     return_adjust);
		pick = zzapp_kernel_dispatch(kernel, source, 0U);
	} else {
		context_save(&kernel->system_context, frame, return_adjust);
		kernel->system_context_valid = 1U;
		pick = zzapp_kernel_schedule(kernel, 0U);
	}
	ACCT_SWITCH(kernel, cur, app, pick);
	ACCT_BOUNDARY_END(kernel);
	/* (The pick spent any yield: zzapp_kernel_schedule.) */
	if ((pick == ZZAPP_KERNEL_SYSTEM || pick == ZZAPP_KERNEL_IDLE) &&
	    kernel->system_context_valid) {
		zzapp_kernel_set_current_thread(-1);
		zzapp_kernel_switch_kernel_address_space();
		return context_restore(&kernel->system_context, frame, return_adjust);
	}
	if (pick < 0)
		return frame;
	zzapp_kernel_set_current_thread(pick);
	/* The SVC vector builds its compact frames at the top of this stack. */
	zzapp_kernel_svc_stack_top =
		(uintptr_t)((const zzapp_rec_t *)thread_at(kernel,
			(uint32_t)pick))->hdr.kstack_top;
	zzapp_kernel_switch_address_space((uint32_t)pick);
#if ZZAPP_PERF_COUNTERS
	kernel_perf_tlb_enter(kernel, pick);
	zzapp_kernel_perf_request_report(kernel, (uint32_t)pick, 0);
#endif
	return context_restore(&thread_at(kernel, (uint32_t)pick)->context,
			       frame, return_adjust);
}



void zzapp_kernel_charge_svc(zzapp_kernel_t *kernel, uint32_t pid,
		uint32_t elapsed_ms)
{
	zzapp_thread_t *thread = kernel_thread(kernel, pid);

	if (!thread || thread->state == ZZAPP_THREAD_BLOCKED)
		return;
	thread->svc_ticks += elapsed_ms;
}

static void wake_api_waiters(zzapp_kernel_t *kernel)
{
	uint32_t i;

	for (i = 0U; i < kernel->run_count; i++) {
		uint32_t pid = kernel->run[i];
		zzapp_rec_kernel_t *words = kernel_words(kernel, pid);

		if (!words->api_waiting && !words->api_resume_waiting)
			continue;
		words->api_waiting = 0U;
		words->api_resume_waiting = 0U;
		words->api_wait_sequence = 0U;
		(void)zzapp_kernel_wake(kernel, pid, ZZAPP_WAKE_READY);
	}
}

static void handoff_api_lock(zzapp_kernel_t *kernel)
{
	uint32_t i;
	uint32_t next = ZZAPP_PID_NONE;

	kernel->api_handoff = ZZAPP_PID_NONE;
	for (i = 0U; i < kernel->run_count; i++) {
		uint32_t pid = kernel->run[i];
		zzapp_rec_kernel_t *words = kernel_words(kernel, pid);

		if (!words->api_waiting ||
		    kernel_thread(kernel, pid)->state == ZZAPP_THREAD_STOPPED)
			continue;
		if (next == ZZAPP_PID_NONE ||
		    (int32_t)(words->api_wait_sequence -
			      kernel_words(kernel, next)->api_wait_sequence) < 0)
			next = pid;
	}
	if (next == ZZAPP_PID_NONE)
		return;
	kernel_words(kernel, next)->api_waiting = 0U;
	kernel_words(kernel, next)->api_wait_sequence = 0U;
	kernel->api_handoff = next;
	(void)zzapp_kernel_wake(kernel, next, ZZAPP_WAKE_READY);
}

static void release_api_lock(zzapp_kernel_t *kernel)
{
	int owner = kernel->api_lock_owner;

	kernel->api_lock_owner = -1;
	kernel->api_lock_depth = 0U;
	/* Preserve a selected waiter while its deferred service pass runs. */
	if (owner == ZZAPP_KERNEL_SYSTEM_OWNER &&
	    kernel->api_handoff != ZZAPP_PID_NONE) {
		kernel->api_handoff_service_deferred = 0U;
		return;
	}
	kernel->api_handoff_service_deferred = 0U;
	handoff_api_lock(kernel);
	/* A service request that arrived during one API body runs before this
	 * reservation, unless its service-window budget is exhausted. */
	if (owner >= 0 && kernel->service_due &&
	    kernel->api_handoff != ZZAPP_PID_NONE)
		kernel->api_handoff_service_deferred = 1U;
}

void zzapp_kernel_reset_lock(zzapp_kernel_t *kernel)
{
	if (!kernel)
		return;
	kernel->api_lock_owner = -1;
	kernel->api_lock_depth = 0U;
	kernel->api_handoff = ZZAPP_PID_NONE;
	kernel->api_handoff_service_deferred = 0U;
	wake_api_waiters(kernel);
}

int zzapp_kernel_system_lock(zzapp_kernel_t *kernel)
{
	if (!kernel)
		return -1;
	if (kernel->api_lock_owner == ZZAPP_KERNEL_SYSTEM_OWNER) {
		kernel->api_lock_depth++;
		return 0;
	}
	if (kernel->api_lock_owner >= 0 ||
	    (kernel->api_handoff != ZZAPP_PID_NONE &&
	     !kernel->api_handoff_service_deferred))
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

int zzapp_kernel_api_fault_release(zzapp_kernel_t *kernel, uint32_t pid)
{
	uint32_t irq_state;

	if (!kernel_thread(kernel, pid))
		return -1;
	irq_state = smp_local_irq_save();
	if (kernel->api_lock_owner != (int)pid) {
		smp_local_irq_restore(irq_state);
		return -1;
	}
	release_api_lock(kernel);
	smp_local_irq_restore(irq_state);
	return 0;
}

int zzapp_kernel_api_lock(zzapp_kernel_t *kernel, uint32_t pid)
{
	zzapp_rec_kernel_t *words = kernel_words(kernel, pid);

	if (!words)
		return -1;
	if (kernel->api_lock_owner == (int)pid) {
		kernel->api_lock_depth++;
		return 0;
	}
	if (kernel->api_lock_owner >= 0 ||
	    kernel->api_lock_owner == ZZAPP_KERNEL_SYSTEM_OWNER ||
	    (kernel->api_handoff != ZZAPP_PID_NONE &&
	     kernel->api_handoff != pid)) {
		if (!words->api_waiting) {
			words->api_waiting = 1U;
			words->api_wait_sequence = ++kernel->api_wait_serial;
		}
		return -1;
	}
	kernel->api_handoff = ZZAPP_PID_NONE;
	kernel->api_handoff_service_deferred = 0U;
	words->api_waiting = 0U;
	words->api_wait_sequence = 0U;
	kernel->api_lock_owner = (int)pid;
	kernel->api_lock_depth = 1U;
	return 0;
}

int zzapp_kernel_api_unlock(zzapp_kernel_t *kernel, uint32_t pid)
{
	uint32_t irq_state;

	if (!kernel_thread(kernel, pid))
		return -1;
	irq_state = smp_local_irq_save();
	if (kernel->api_lock_owner != (int)pid) {
		smp_local_irq_restore(irq_state);
		return -1;
	}
	if (--kernel->api_lock_depth == 0U)
		release_api_lock(kernel);
	smp_local_irq_restore(irq_state);
	return 0;
}

/*
 * The lock as nearly every GL call takes and gives it: nobody holds it, nobody
 * waits for it, or the caller already holds it. These do what zzapp_kernel_api_lock
 * and zzapp_kernel_api_unlock do in that case, and nothing else; they return 0
 * without changing anything when the case is another one, and the caller then
 * takes the general path.
 *
 * A take from free is the one step that needs the IRQ mask: a tick between the
 * test and the stores could hand the system thread the lock. The mask is one
 * instruction each way. A nested take only adds to the depth, and the depth is
 * the holder's own word. A last release with the only app thread on the run
 * list has no waiter to hand the lock to (the system thread polls and never
 * waits), so it is two stores, the depth first: a tick between them still sees
 * the app as the holder, and the system thread cannot take a lock it holds.
 */
#ifdef ZZAPP_HOST_TEST
#define KERNEL_IRQ_OFF() smp_local_irq_save()
#define KERNEL_IRQ_RESTORE(state) smp_local_irq_restore(state)
#else
static inline uint32_t kernel_irq_off(void)
{
	uint32_t cpsr;

	__asm__ __volatile__("mrs %0, cpsr\n\tcpsid i" : "=r"(cpsr) : : "memory");
	return cpsr;
}

static inline void kernel_irq_restore(uint32_t cpsr)
{
	if ((cpsr & 0x80U) == 0U)
		__asm__ __volatile__("cpsie i" : : : "memory");
}
#define KERNEL_IRQ_OFF() kernel_irq_off()
#define KERNEL_IRQ_RESTORE(state) kernel_irq_restore(state)
#endif

int zzapp_kernel_api_try_lock(zzapp_kernel_t *kernel, uint32_t pid)
{
	zzapp_rec_kernel_t *words;
	uint32_t irq_state;

	if (kernel->api_lock_owner == (int)pid) {
		kernel->api_lock_depth++;
		return 1;
	}
	if (kernel->api_lock_owner != -1 || kernel->api_handoff != ZZAPP_PID_NONE)
		return 0;
	words = kernel_words(kernel, pid);
	if (!words)
		return 0;
	irq_state = KERNEL_IRQ_OFF();
	if (kernel->api_lock_owner != -1 || kernel->api_handoff != ZZAPP_PID_NONE) {
		KERNEL_IRQ_RESTORE(irq_state);
		return 0;
	}
	kernel->api_handoff_service_deferred = 0U;
	words->api_waiting = 0U;
	words->api_wait_sequence = 0U;
	kernel->api_lock_depth = 1U;
	kernel->api_lock_owner = (int)pid;
	KERNEL_IRQ_RESTORE(irq_state);
	return 1;
}

int zzapp_kernel_api_try_unlock(zzapp_kernel_t *kernel, uint32_t pid)
{
	if (kernel->api_lock_owner != (int)pid)
		return 0;
	if (kernel->api_lock_depth > 1U) {
		kernel->api_lock_depth--;
		return 1;
	}
	if (kernel->api_lock_depth == 1U && kernel->run_count == 1U &&
	    kernel->api_handoff == ZZAPP_PID_NONE) {
		kernel->api_lock_depth = 0U;
		__asm__ __volatile__("" : : : "memory");
		kernel->api_handoff_service_deferred = 0U;
		kernel->api_lock_owner = -1;
		return 1;
	}
	return 0;
}

/* An API wait releases the lock before parking.  Reserve the next handoff
 * only after its event wakes it, so the service thread cannot overtake the
 * suspended API body or spuriously complete an unready wait. */
static void register_api_waiter(zzapp_kernel_t *kernel, uint32_t pid)
{
	zzapp_rec_kernel_t *words = kernel_words(kernel, pid);

	if (!words->api_waiting) {
		words->api_waiting = 1U;
		words->api_wait_sequence = ++kernel->api_wait_serial;
	}
}

int zzapp_kernel_api_block(zzapp_kernel_t *kernel, uint32_t pid)
{
	uint32_t irq_state;
	int rc;

	if (!kernel_thread(kernel, pid))
		return -1;
	irq_state = smp_local_irq_save();
	if (kernel->api_lock_owner == (int)pid)
		release_api_lock(kernel);
	rc = zzapp_kernel_block(kernel, pid);
	if (rc == 0)
		kernel_words(kernel, pid)->api_resume_waiting = 1U;
	smp_local_irq_restore(irq_state);
	return rc;
}

/* The kernel clock, in whole milliseconds, as of the microsecond reading
 * `now_us`: the committed count plus the whole milliseconds of the clock
 * since the slice began (the same difference of floored readings the tick
 * adds). Unlike zzapp_kernel_now it counts an idle core as well, so a
 * deadline set from it is always on the clock's own millisecond boundaries. */
static uint64_t kernel_ms_at(const zzapp_kernel_t *kernel, uint64_t now_us)
{
	uint32_t elapsed_ms;
	uint32_t elapsed_us;

	slice_elapsed(kernel, now_us, &elapsed_ms, &elapsed_us);
	return kernel->now + elapsed_ms;
}

/*
 * The sleep lasts at least `sleep_us` of the microsecond clock and ends at the
 * first look at that clock after it: the thread's microsecond deadline is
 * `now_us + sleep_us`, and wake_due compares it to the clock, so whatever
 * advances or polls the kernel wakes it within that look's latency of the
 * deadline. The millisecond deadline stays as the tick-granular backstop. It
 * is the first boundary at or after the microsecond deadline: the clock
 * steps on the microsecond reading's millisecond boundaries, so a deadline of
 * `now_ms + ceil(sleep_us / 1000)` would end the sleep early by the fraction
 * of the millisecond already elapsed when it began. One clock reading, under
 * the IRQ mask, gives the count, the fraction and the microsecond deadline.
 */
int zzapp_kernel_api_sleep(zzapp_kernel_t *kernel, uint32_t pid, uint64_t sleep_us)
{
	uint32_t irq_state;
	uint64_t now_us;
	uint64_t deadline;

	if (!kernel_thread(kernel, pid))
		return -1;
	irq_state = smp_local_irq_save();
	if (kernel->api_lock_owner == (int)pid)
		release_api_lock(kernel);
	now_us = zzapp_kernel_now_us();
	deadline = kernel_ms_at(kernel, now_us) +
		   (sleep_us + now_us % 1000U + 999U) / 1000U;
	if (zzapp_kernel_sleep(kernel, pid, deadline) == 0) {
		kernel_thread(kernel, pid)->sleep_deadline_us = now_us + sleep_us;
		kernel_words(kernel, pid)->api_resume_waiting = 1U;
	}
	smp_local_irq_restore(irq_state);
	return 0;
}

/*
 * Wakes the sleepers whose microsecond deadline has passed, for the system
 * thread's loop (the timer tick does it through the advance). Returns 1 when
 * a sleeper's deadline is within `horizon_us` of the clock, so the loop keeps
 * looking at the clock instead of idling until the next tick.
 */
int zzapp_kernel_sleep_poll(zzapp_kernel_t *kernel, uint32_t horizon_us)
{
	uint32_t irq_state;
	uint64_t now_us = 0U;
	int have_now = 0;
	int near = 0;
	uint32_t i;

	if (!kernel)
		return 0;
	irq_state = smp_local_irq_save();
	wake_due(kernel);
	for (i = 0U; i < kernel->run_count; i++) {
		const zzapp_thread_t *thread = kernel_thread(kernel, kernel->run[i]);

		if (thread->state != ZZAPP_THREAD_SLEEPING ||
		    thread->sleep_deadline_us == 0U)
			continue;
		if (!have_now) {
			now_us = zzapp_kernel_now_us();
			have_now = 1;
		}
		if (thread->sleep_deadline_us <= now_us + horizon_us)
			near = 1;
	}
	smp_local_irq_restore(irq_state);
	return near;
}

static int surface_overlaps(uint32_t base_a, uint32_t bytes_a,
			    uint32_t base_b, uint32_t bytes_b)
{
	uint32_t end_a = base_a + bytes_a;
	uint32_t end_b = base_b + bytes_b;

	return end_a > base_b && end_b > base_a;
}

int zzapp_kernel_surface_bind(zzapp_kernel_t *kernel, uint32_t pid,
			      uint32_t generation, uint32_t base, uint32_t bytes)
{
	zzapp_kernel_resources_t *resource;
	uint32_t i;

	if (!kernel_thread(kernel, pid) || !base || !bytes ||
	    (base & (ZZAPP_KERNEL_PAGE_BYTES - 1U)) != 0U ||
	    (bytes & (ZZAPP_KERNEL_PAGE_BYTES - 1U)) != 0U ||
	    base + bytes < base)
		return -1;
	resource = &kernel_words(kernel, pid)->resources;
	if (resource->surface_bytes != 0U)
		return resource->surface_base == base &&
		       resource->surface_bytes == bytes &&
		       resource->surface_generation == generation ? 0 : -1;
	for (i = 0U; i < kernel->capacity; i++) {
		const zzapp_kernel_resources_t *other =
			&kernel_words(kernel, i)->resources;

		if (i != pid && other->surface_bytes != 0U &&
		    surface_overlaps(base, bytes, other->surface_base,
				     other->surface_bytes))
			return -1;
	}
	resource->surface_base = base;
	resource->surface_bytes = bytes;
	resource->surface_generation = generation;
	return 0;
}
void zzapp_kernel_surface_unbind(zzapp_kernel_t *kernel, uint32_t pid)
{
	zzapp_rec_kernel_t *words = kernel_words(kernel, pid);

	if (!words)
		return;
	words->resources.surface_base = 0U;
	words->resources.surface_bytes = 0U;
	words->resources.surface_generation = 0U;
}


int zzapp_kernel_audio_open(zzapp_kernel_t *kernel, uint32_t pid,
			    int direct_card_route)
{
	zzapp_rec_kernel_t *words = kernel_words(kernel, pid);
	zzapp_kernel_resources_t *resource;

	if (!words)
		return -1;
	resource = &words->resources;
	if (resource->direct_audio)
		return -1;
	if (direct_card_route && kernel->direct_audio_count >= 2U)
		return -1;
	resource->direct_audio = direct_card_route ? 1U : 2U;
	if (direct_card_route)
		kernel->direct_audio_count++;
	return 0;
}

void zzapp_kernel_audio_close(zzapp_kernel_t *kernel, uint32_t pid)
{
	zzapp_rec_kernel_t *words = kernel_words(kernel, pid);
	zzapp_kernel_resources_t *resource;

	if (!words)
		return;
	resource = &words->resources;
	if (resource->direct_audio == 1U && kernel->direct_audio_count != 0U)
		kernel->direct_audio_count--;
	resource->direct_audio = 0U;
}

int zzapp_kernel_gl_bind(zzapp_kernel_t *kernel, uint32_t pid,
			 uint32_t generation)
{
	zzapp_rec_kernel_t *words = kernel_words(kernel, pid);

	if (!words)
		return -1;
	if (kernel->gl_owner >= 0)
		return kernel->gl_owner == (int)pid &&
		       kernel->gl_generation == generation ? 0 : -1;
	kernel->gl_owner = (int)pid;
	kernel->gl_generation = generation;
	words->resources.gl_generation = generation;
	return 0;
}

void zzapp_kernel_gl_unbind(zzapp_kernel_t *kernel, uint32_t pid,
			    uint32_t generation)
{
	zzapp_rec_kernel_t *words = kernel_words(kernel, pid);

	if (words && kernel->gl_owner == (int)pid &&
	    kernel->gl_generation == generation) {
		kernel->gl_owner = -1;
		kernel->gl_generation = 0U;
		words->resources.gl_generation = 0U;
	}
}

int zzapp_kernel_session_open(zzapp_kernel_t *kernel, uint32_t pid,
			      int audio_session)
{
	zzapp_rec_kernel_t *words = kernel_words(kernel, pid);
	uint32_t *sessions;

	if (!words)
		return -1;
	sessions = audio_session ? &words->resources.audio_sessions :
				   &words->resources.image_sessions;
	if (*sessions >= ZZAPP_KERNEL_SESSION_LIMIT)
		return -1;
	(*sessions)++;
	return 0;
}

void zzapp_kernel_session_close(zzapp_kernel_t *kernel, uint32_t pid,
			       int audio_session)
{
	zzapp_rec_kernel_t *words = kernel_words(kernel, pid);
	uint32_t *sessions;

	if (!words)
		return;
	sessions = audio_session ? &words->resources.audio_sessions :
				   &words->resources.image_sessions;
	if (*sessions != 0U)
		(*sessions)--;
}

void zzapp_kernel_stop_app(zzapp_kernel_t *kernel, uint32_t pid)
{
	zzapp_thread_t *thread = kernel_thread(kernel, pid);
	zzapp_rec_kernel_t *words = kernel_words(kernel, pid);

	if (!thread)
		return;
	ACCT_STOP(kernel, thread);
	if (kernel->yielder == pid)
		yield_spent(kernel);
	zzapp_kernel_audio_close(kernel, pid);
	zzapp_kernel_gl_unbind(kernel, pid, words->resources.gl_generation);
	memset(&words->resources, 0, sizeof(words->resources));
	thread->wake_reason = ZZAPP_WAKE_CANCELLED;
	thread->state = ZZAPP_THREAD_STOPPED;
	thread->quantum_left = 0U;
	run_remove(kernel, pid);
	if (kernel->active == pid)
		kernel->active = ZZAPP_PID_NONE;
	if (kernel->api_lock_owner == (int)pid)
		release_api_lock(kernel);
	else if (kernel->api_handoff == pid) {
		kernel->api_handoff = ZZAPP_PID_NONE;
		handoff_api_lock(kernel);
	}
	words->api_waiting = 0U;
	words->api_resume_waiting = 0U;
	words->api_wait_sequence = 0U;
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
