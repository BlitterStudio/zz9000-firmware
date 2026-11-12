/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "zzapp_kernel.h"

static unsigned irq_save_calls;
static unsigned irq_restore_calls;

uint32_t smp_local_irq_save(void)
{
	irq_save_calls++;
	return 0U;
}

void smp_local_irq_restore(uint32_t state)
{
	(void)state;
	irq_restore_calls++;
}

static int failures;
static unsigned checks;

#define CHECK(x) do { \
	checks++; \
	if (!(x)) { \
		failures++; \
		printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
	} \
} while (0)

static void make_runnable(zzapp_kernel_t *kernel, unsigned slot)
{
	CHECK(zzapp_kernel_start_app(kernel, slot) == 0);
}

static void test_first_context_and_dispatch(void)
{
	zzapp_kernel_t kernel;
	unsigned slot;

	zzapp_kernel_init(&kernel);
	CHECK(zzapp_kernel_prepare_app(&kernel, 0, 0x12345678u,
				       0x23456789u, 3u, 0x34567890u,
				       0x45678000u) == 0);
	CHECK(kernel.threads[0].context.r[15] == 0x12345678u);
	CHECK(kernel.threads[0].context.r[0] == 0x23456789u);
	CHECK(kernel.threads[0].context.r[1] == 3u);
	CHECK(kernel.threads[0].context.r[2] == 0x34567890u);
	CHECK(kernel.threads[0].context.r[13] == 0x45678000u);
	CHECK(kernel.threads[0].context.cpsr == ZZAPP_KERNEL_APP_CPSR);
	CHECK(kernel.threads[0].context.fpscr == 0u);
	for (slot = 0; slot < ZZAPP_KERNEL_APP_SLOTS; slot++)
		make_runnable(&kernel, slot);
	CHECK(zzapp_kernel_dispatch(&kernel, ZZAPP_KERNEL_DISPATCH_TICK, 5u) ==
	      ZZAPP_KERNEL_APP0);
	CHECK(zzapp_kernel_dispatch(&kernel, ZZAPP_KERNEL_DISPATCH_TICK, 5u) ==
	      ZZAPP_KERNEL_APP1);
	CHECK(zzapp_kernel_dispatch(&kernel, ZZAPP_KERNEL_DISPATCH_TICK, 5u) ==
	      ZZAPP_KERNEL_APP2);
	CHECK(zzapp_kernel_dispatch(&kernel, ZZAPP_KERNEL_DISPATCH_TICK, 5u) ==
	      ZZAPP_KERNEL_APP3);
	zzapp_kernel_service_due(&kernel);
	CHECK(zzapp_kernel_dispatch(&kernel, ZZAPP_KERNEL_DISPATCH_DOORBELL, 0u) ==
	      ZZAPP_KERNEL_SYSTEM);
	CHECK(zzapp_kernel_dispatch(&kernel, ZZAPP_KERNEL_DISPATCH_VOLUNTARY, 0u) ==
	      ZZAPP_KERNEL_APP0);
}

static void test_frame_switch(void)
{
	zzapp_kernel_t kernel;
	zzapp_arm_frame_t frame;
	zzapp_arm_frame_t *next;

	zzapp_kernel_init(&kernel);
	CHECK(zzapp_kernel_prepare_app(&kernel, 0, 0x1000u, 1u, 0u, 0u,
				       0x8000u) == 0);
	CHECK(zzapp_kernel_prepare_app(&kernel, 1, 0x2000u, 2u, 0u, 0u,
				       0x9000u) == 0);
	make_runnable(&kernel, 0);
	make_runnable(&kernel, 1);
	frame = kernel.threads[0].context.frame;
	frame.exception_lr = 0x1004u;
	zzapp_kernel_set_current_thread(0);
	next = zzapp_kernel_switch_frame(&kernel, &frame,
					 ZZAPP_KERNEL_DISPATCH_TICK, 4u);
	CHECK(next == &kernel.threads[0].context.frame);
	CHECK(kernel.threads[0].context.r[15] == 0x1000u);
	CHECK(kernel.threads[0].context.r[13] == 0x8000u);
	next = zzapp_kernel_switch_frame(&kernel, next,
					 ZZAPP_KERNEL_DISPATCH_TICK, 4u);
	CHECK(next == &kernel.threads[1].context.frame);
	CHECK(zzapp_kernel_current_thread() == 1);
}

static void test_system_frame_runs_service_before_apps(void)
{
	zzapp_kernel_t kernel;
	zzapp_arm_frame_t system_frame = { 0 };
	zzapp_arm_frame_t *next;

	zzapp_kernel_init(&kernel);
	CHECK(zzapp_kernel_prepare_app(&kernel, 0, 0x1000u, 1u, 0u, 0u,
				       0x8000u) == 0);
	make_runnable(&kernel, 0);
	system_frame.exception_lr = 0x5000u;
	system_frame.spsr = ZZAPP_KERNEL_APP_CPSR;
	zzapp_kernel_set_current_thread(-1);
	next = zzapp_kernel_switch_frame(&kernel, &system_frame,
					 ZZAPP_KERNEL_DISPATCH_VOLUNTARY, 0u);
	CHECK(next == &kernel.threads[0].context.frame);
	zzapp_kernel_service_due(&kernel);
	next = zzapp_kernel_switch_frame(&kernel, next,
					 ZZAPP_KERNEL_DISPATCH_DOORBELL, 0u);
	CHECK(next == &kernel.system_context.frame);
	CHECK(zzapp_kernel_current_thread() == -1);
}

static void test_round_robin_quantum(void)
{
	zzapp_kernel_t kernel;
	unsigned i;

	zzapp_kernel_init(&kernel);
	make_runnable(&kernel, 0);
	make_runnable(&kernel, 1);
	for (i = 0; i < 400; i++)
		(void)zzapp_kernel_schedule(&kernel, 5);
	CHECK(kernel.threads[0].user_ticks == kernel.threads[1].user_ticks);
	CHECK(kernel.threads[0].user_ticks == 1000u);
}

/* Kit 66: every tick also makes service due, so it hands the core to the
 * system thread, which yields back to an app after its pass. With two
 * CPU-bound spin apps one of them got ~95% of the core. Both must get the
 * same share when every tick goes through the system thread. */
static void test_ticks_through_system_thread_share_fairly(void)
{
	zzapp_kernel_t kernel;
	zzapp_arm_frame_t frame = { 0 };
	zzapp_arm_frame_t *next;
	unsigned runs[2] = { 0u, 0u };
	unsigned i;

	zzapp_kernel_init(&kernel);
	CHECK(zzapp_kernel_prepare_app(&kernel, 0, 0x1000u, 1u, 0u, 0u,
				       0x8000u) == 0);
	CHECK(zzapp_kernel_prepare_app(&kernel, 1, 0x2000u, 2u, 0u, 0u,
				       0x9000u) == 0);
	make_runnable(&kernel, 0);
	make_runnable(&kernel, 1);
	frame.spsr = ZZAPP_KERNEL_APP_CPSR;
	zzapp_kernel_set_current_thread(-1);
	next = zzapp_kernel_switch_frame(&kernel, &frame,
					 ZZAPP_KERNEL_DISPATCH_VOLUNTARY, 0u);
	for (i = 0; i < 200; i++) {
		int app = zzapp_kernel_current_thread();

		CHECK(app == 0 || app == 1);
		if (app != 0 && app != 1)
			return;
		runs[app]++;
		/* The timer tick marks service due and lands in the app. The
		 * scheduler hands it to the system thread unless the 95% cap
		 * is spent; the system thread then finishes its pass and
		 * yields. */
		zzapp_kernel_service_due(&kernel);
		next = zzapp_kernel_switch_frame(&kernel, next,
						 ZZAPP_KERNEL_DISPATCH_TICK, 4u);
		if (zzapp_kernel_current_thread() == -1)
			next = zzapp_kernel_switch_frame(&kernel, next,
					ZZAPP_KERNEL_DISPATCH_VOLUNTARY, 0u);
	}
	CHECK(runs[0] + runs[1] == 200u);
	CHECK(runs[0] >= 95u && runs[1] >= 95u);
}

static void test_service_precedes_app_and_preserves_remainder(void)
{
	zzapp_kernel_t kernel;

	zzapp_kernel_init(&kernel);
	make_runnable(&kernel, 0);
	CHECK(zzapp_kernel_schedule(&kernel, 2) == ZZAPP_KERNEL_APP0);
	zzapp_kernel_service_due(&kernel);
	CHECK(zzapp_kernel_schedule(&kernel, 1) == ZZAPP_KERNEL_SYSTEM);
	CHECK(zzapp_kernel_schedule(&kernel, 3) == ZZAPP_KERNEL_APP0);
	CHECK(kernel.threads[0].quantum_left == 0u);
}

static void test_block_wake_and_idle(void)
{
	zzapp_kernel_t kernel;

	zzapp_kernel_init(&kernel);
	make_runnable(&kernel, 0);
	CHECK(zzapp_kernel_block(&kernel, 0) == 0);
	CHECK(zzapp_kernel_schedule(&kernel, 5) == ZZAPP_KERNEL_IDLE);
	CHECK(zzapp_kernel_wake(&kernel, 0, ZZAPP_WAKE_CANCELLED) == 0);
	CHECK(zzapp_kernel_schedule(&kernel, 5) == ZZAPP_KERNEL_APP0);
	CHECK(kernel.threads[0].wake_reason == ZZAPP_WAKE_CANCELLED);
}

static void test_sleep_deadline_order(void)
{
	zzapp_kernel_t kernel;

	zzapp_kernel_init(&kernel);
	make_runnable(&kernel, 0);
	make_runnable(&kernel, 1);
	CHECK(zzapp_kernel_sleep(&kernel, 1, 10) == 0);
	CHECK(zzapp_kernel_sleep(&kernel, 0, 10) == 0);
	zzapp_kernel_advance(&kernel, 10);
	CHECK(kernel.threads[1].state == ZZAPP_THREAD_RUNNABLE);
	CHECK(kernel.threads[0].state == ZZAPP_THREAD_RUNNABLE);
	CHECK(kernel.wake_log_count == 2u);
	CHECK(kernel.wake_log[0] == 1u);
	CHECK(kernel.wake_log[1] == 0u);
	CHECK(zzapp_kernel_schedule(&kernel, 5) != ZZAPP_KERNEL_IDLE);
}

static void test_wait_completion_timeout_and_cancel(void)
{
	zzapp_kernel_t kernel;

	zzapp_kernel_init(&kernel);
	make_runnable(&kernel, 0);
	CHECK(zzapp_kernel_block(&kernel, 0) == 0);
	CHECK(zzapp_kernel_wake(&kernel, 0, ZZAPP_WAKE_READY) == 0);
	CHECK(zzapp_kernel_wait_status(&kernel, 0) == 0);
	CHECK(zzapp_kernel_block(&kernel, 0) == 0);
	CHECK(zzapp_kernel_wake(&kernel, 0, ZZAPP_WAKE_TIMEOUT) == 0);
	CHECK(zzapp_kernel_wait_status(&kernel, 0) ==
	      ZZAPP_KERNEL_WAIT_TIMEOUT);
	CHECK(zzapp_kernel_block(&kernel, 0) == 0);
	CHECK(zzapp_kernel_wake(&kernel, 0, ZZAPP_WAKE_CANCELLED) == 0);
	CHECK(zzapp_kernel_wait_status(&kernel, 0) ==
	      ZZAPP_KERNEL_WAIT_CANCELLED);
}

static void test_accounting_and_throttle(void)
{
	zzapp_kernel_t kernel;
	unsigned i;

	zzapp_kernel_init(&kernel);
	make_runnable(&kernel, 0);
	CHECK(zzapp_kernel_schedule(&kernel, 2) == ZZAPP_KERNEL_APP0);
	zzapp_kernel_charge_svc(&kernel, 0, 3);
	CHECK(kernel.threads[0].user_ticks + kernel.threads[0].svc_ticks == 5u);
	CHECK(zzapp_kernel_block(&kernel, 0) == 0);
	(void)zzapp_kernel_schedule(&kernel, 5);
	CHECK(kernel.threads[0].user_ticks + kernel.threads[0].svc_ticks == 5u);

	zzapp_kernel_init(&kernel);
	make_runnable(&kernel, 0);
	for (i = 0; i < 200; i++) {
		zzapp_kernel_service_due(&kernel);
		(void)zzapp_kernel_schedule(&kernel, 5);
	}
	CHECK(kernel.threads[0].user_ticks >= 10u);
	CHECK(kernel.overload_count != 0u);
	CHECK(kernel.service_ticks > kernel.threads[0].user_ticks);
	CHECK(zzapp_kernel_schedule(&kernel, 0) != ZZAPP_KERNEL_IDLE);
}

static void test_blocking_api_lock_and_slot_resources(void)
{
	zzapp_kernel_t kernel;
	unsigned i;

	zzapp_kernel_init(&kernel);
	make_runnable(&kernel, 0);
	make_runnable(&kernel, 1);
	CHECK(zzapp_kernel_api_lock(&kernel, 0) == 0);
	CHECK(zzapp_kernel_api_lock(&kernel, 1) != 0);
	CHECK(zzapp_kernel_api_block(&kernel, 0) == 0);
	CHECK(kernel.api_lock_owner == -1);
	CHECK(kernel.threads[0].state == ZZAPP_THREAD_BLOCKED);
	CHECK(zzapp_kernel_api_lock(&kernel, 1) == 0);
	for (i = 0; i < 20; i++)
		(void)zzapp_kernel_schedule(&kernel, 5);
	CHECK(kernel.threads[0].user_ticks == 0u);
	irq_save_calls = 0U;
	irq_restore_calls = 0U;
	CHECK(zzapp_kernel_api_unlock(&kernel, 1) == 0);
	CHECK(irq_save_calls == 1U);
	CHECK(irq_restore_calls == 1U);
	CHECK(kernel.threads[0].wake_reason == ZZAPP_WAKE_READY);

	CHECK(zzapp_kernel_surface_bind(&kernel, 0, 11u, 0x200000u,
					 0x2000u) == 0);
	CHECK(zzapp_kernel_surface_bind(&kernel, 1, 12u, 0x201000u,
					 0x1000u) != 0);
	CHECK(zzapp_kernel_surface_bind(&kernel, 1, 12u, 0x202000u,
					 0x1000u) == 0);
	CHECK(zzapp_kernel_audio_open(&kernel, 0, 1) == 0);
	CHECK(zzapp_kernel_audio_open(&kernel, 1, 1) == 0);
	CHECK(zzapp_kernel_audio_open(&kernel, 2, 1) != 0);
	zzapp_kernel_audio_close(&kernel, 1);
	CHECK(zzapp_kernel_audio_open(&kernel, 2, 1) == 0);

	CHECK(zzapp_kernel_gl_bind(&kernel, 0, 11u) == 0);
	CHECK(zzapp_kernel_gl_bind(&kernel, 1, 12u) != 0);
	zzapp_kernel_stop_app(&kernel, 0);
	CHECK(zzapp_kernel_gl_bind(&kernel, 1, 12u) == 0);
	CHECK(kernel.resources[1].surface_bytes == 0x1000u);
	for (i = 0; i < ZZAPP_KERNEL_SESSION_LIMIT; i++)
		CHECK(zzapp_kernel_session_open(&kernel, 1, 0) == 0);
	CHECK(zzapp_kernel_session_open(&kernel, 1, 0) != 0);
	for (i = 0; i < ZZAPP_KERNEL_SESSION_LIMIT; i++)
		CHECK(zzapp_kernel_session_open(&kernel, 1, 1) == 0);
	CHECK(zzapp_kernel_session_open(&kernel, 1, 1) != 0);
	zzapp_kernel_stop_app(&kernel, 1);
	CHECK(kernel.resources[1].surface_bytes == 0u);
	CHECK(kernel.resources[1].image_sessions == 0u);
	CHECK(kernel.resources[1].audio_sessions == 0u);
}

static void test_waiter_isolation_timeout_and_stop(void)
{
	zzapp_kernel_t kernel;

	zzapp_kernel_init(&kernel);
	make_runnable(&kernel, 1);
	make_runnable(&kernel, 2);
	CHECK(zzapp_kernel_api_lock(&kernel, 1) == 0);
	CHECK(zzapp_kernel_api_block(&kernel, 1) == 0);
	CHECK(zzapp_kernel_schedule(&kernel, 5) == ZZAPP_KERNEL_APP2);
	CHECK(zzapp_kernel_wake(&kernel, 1, ZZAPP_WAKE_READY) == 0);
	CHECK(kernel.wake_log_count == 1u);
	CHECK(kernel.wake_log[0] == 1u);
	CHECK(kernel.threads[2].state == ZZAPP_THREAD_RUNNABLE);

	CHECK(zzapp_kernel_sleep(&kernel, 1, kernel.now + 10u) == 0);
	zzapp_kernel_advance(&kernel, 10u);
	CHECK(zzapp_kernel_wait_status(&kernel, 1) == ZZAPP_KERNEL_WAIT_TIMEOUT);

	CHECK(zzapp_kernel_block(&kernel, 1) == 0);
	zzapp_kernel_stop_app(&kernel, 1);
	CHECK(zzapp_kernel_wait_status(&kernel, 1) == ZZAPP_KERNEL_WAIT_CANCELLED);
}

static void test_stop_wakes_api_lock_waiter(void)
{
	zzapp_kernel_t kernel;

	zzapp_kernel_init(&kernel);
	make_runnable(&kernel, 0);
	make_runnable(&kernel, 1);
	CHECK(zzapp_kernel_api_lock(&kernel, 0) == 0);
	CHECK(zzapp_kernel_api_lock(&kernel, 1) != 0);
	CHECK(zzapp_kernel_block(&kernel, 1) == 0);
	zzapp_kernel_stop_app(&kernel, 0);
	CHECK(kernel.threads[1].state == ZZAPP_THREAD_RUNNABLE);
	CHECK(kernel.threads[1].wake_reason == ZZAPP_WAKE_READY);
}

static void test_system_kernel_lock_handoffs(void)
{
	zzapp_kernel_t kernel;

	zzapp_kernel_init(&kernel);
	make_runnable(&kernel, 0);
	CHECK(zzapp_kernel_system_lock(&kernel) == 0);
	CHECK(kernel.api_lock_owner == ZZAPP_KERNEL_SYSTEM_OWNER);
	CHECK(zzapp_kernel_api_lock(&kernel, 0) != 0);
	CHECK(zzapp_kernel_api_block(&kernel, 0) == 0);
	CHECK(kernel.threads[0].state == ZZAPP_THREAD_BLOCKED);
	CHECK(zzapp_kernel_schedule(&kernel, 0) == ZZAPP_KERNEL_SYSTEM);
	CHECK(zzapp_kernel_system_unlock(&kernel) == 0);
	CHECK(kernel.threads[0].state == ZZAPP_THREAD_RUNNABLE);
	CHECK(zzapp_kernel_api_lock(&kernel, 0) == 0);
	CHECK(zzapp_kernel_api_unlock(&kernel, 0) == 0);

	zzapp_kernel_init(&kernel);
	make_runnable(&kernel, 0);
	CHECK(zzapp_kernel_api_lock(&kernel, 0) == 0);
	zzapp_kernel_service_due(&kernel);
	CHECK(zzapp_kernel_schedule(&kernel, 0) == ZZAPP_KERNEL_APP0);
	CHECK(kernel.service_due != 0U);
	CHECK(zzapp_kernel_api_unlock(&kernel, 0) == 0);
	CHECK(zzapp_kernel_schedule(&kernel, 0) == ZZAPP_KERNEL_SYSTEM);

	zzapp_kernel_init(&kernel);
	make_runnable(&kernel, 0);
	CHECK(zzapp_kernel_api_lock(&kernel, 0) == 0);
	CHECK(zzapp_kernel_system_lock(&kernel) != 0);
	CHECK(zzapp_kernel_schedule(&kernel, 0) == ZZAPP_KERNEL_APP0);
	CHECK(zzapp_kernel_api_unlock(&kernel, 0) == 0);
}

static void test_fault_releases_kernel_lock(void)
{
	zzapp_kernel_t kernel;

	zzapp_kernel_init(&kernel);
	make_runnable(&kernel, 0);
	make_runnable(&kernel, 1);
	CHECK(zzapp_kernel_api_lock(&kernel, 0) == 0);
	CHECK(zzapp_kernel_api_lock(&kernel, 1) != 0);
	CHECK(zzapp_kernel_api_block(&kernel, 1) == 0);
	CHECK(zzapp_kernel_api_fault_release(&kernel, 0) == 0);
	CHECK(kernel.api_lock_owner == -1);
	CHECK(kernel.threads[1].state == ZZAPP_THREAD_RUNNABLE);
}

static void test_kernel_stack_covers_decoder_call_chain(void)
{
	/* MP3's decode scratch alone is 13,216 bytes; leave room for the
	 * exception frame and its callers without sharing a stack between apps. */
	CHECK(ZZAPP_KERNEL_STACK_BYTES >= 32768U);
}

static void test_non_timer_interrupt_does_not_advance_time(void)
{
	zzapp_kernel_t kernel;
	zzapp_arm_frame_t frame = { 0 };

	zzapp_kernel_init(&kernel);
	(void)zzapp_kernel_switch_frame(&kernel, &frame,
		ZZAPP_KERNEL_DISPATCH_INTERRUPT, 4U);
	CHECK(kernel.now == 0U);
}

static void test_system_ticks_wake_sleepers(void)
{
	zzapp_kernel_t kernel;
	zzapp_arm_frame_t system_frame = { 0 };
	zzapp_arm_frame_t *next;

	zzapp_kernel_init(&kernel);
	CHECK(zzapp_kernel_prepare_app(&kernel, 0, 0x1000u, 1u, 0u, 0u,
				       0x8000u) == 0);
	make_runnable(&kernel, 0);
	CHECK(zzapp_kernel_sleep(&kernel, 0, 10u) == 0);
	zzapp_kernel_set_current_thread(-1);
	next = zzapp_kernel_switch_frame(&kernel, &system_frame,
					 ZZAPP_KERNEL_DISPATCH_TICK, 4u);
	CHECK(next == &kernel.system_context.frame);
	CHECK(kernel.now == 5u);
	CHECK(kernel.threads[0].state == ZZAPP_THREAD_SLEEPING);
	next = zzapp_kernel_switch_frame(&kernel, next,
					 ZZAPP_KERNEL_DISPATCH_TICK, 4u);
	CHECK(kernel.now == 10u);
	CHECK(next == &kernel.threads[0].context.frame);
	CHECK(kernel.threads[0].state == ZZAPP_THREAD_RUNNABLE);
}
static void test_restart_releases_kernel_lock_waiters(void)
{
	zzapp_kernel_t kernel;

	zzapp_kernel_init(&kernel);
	make_runnable(&kernel, 0);
	make_runnable(&kernel, 1);
	CHECK(zzapp_kernel_system_lock(&kernel) == 0);
	CHECK(zzapp_kernel_api_lock(&kernel, 0) != 0);
	CHECK(zzapp_kernel_api_block(&kernel, 0) == 0);
	zzapp_kernel_reset_lock(&kernel);
	CHECK(kernel.api_lock_owner == -1);
	CHECK(kernel.api_lock_depth == 0U);
	CHECK(kernel.api_waiting[0] == 0U);
	CHECK(kernel.threads[0].state == ZZAPP_THREAD_RUNNABLE);
	CHECK(kernel.threads[0].wake_reason == ZZAPP_WAKE_READY);

	CHECK(zzapp_kernel_api_lock(&kernel, 0) == 0);
	CHECK(zzapp_kernel_api_lock(&kernel, 1) != 0);
	CHECK(zzapp_kernel_api_block(&kernel, 1) == 0);
	zzapp_kernel_reset_lock(&kernel);
	CHECK(kernel.api_lock_owner == -1);
	CHECK(kernel.api_lock_depth == 0U);
	CHECK(kernel.api_waiting[1] == 0U);
	CHECK(kernel.threads[1].state == ZZAPP_THREAD_RUNNABLE);
	CHECK(kernel.threads[1].wake_reason == ZZAPP_WAKE_READY);
}

static void test_system_lock_inheritance_is_throttled(void)
{
	zzapp_kernel_t kernel;
	unsigned i;

	zzapp_kernel_init(&kernel);
	make_runnable(&kernel, 0);
	make_runnable(&kernel, 1);
	CHECK(zzapp_kernel_system_lock(&kernel) == 0);
	CHECK(zzapp_kernel_api_lock(&kernel, 0) != 0);
	CHECK(zzapp_kernel_api_block(&kernel, 0) == 0);
	for (i = 0U; i < 19U; ++i)
		CHECK(zzapp_kernel_schedule(&kernel, 5U) == ZZAPP_KERNEL_SYSTEM);
	CHECK(kernel.service_ticks == 95U);
	CHECK(kernel.service_in_window == 95U);
	CHECK(zzapp_kernel_schedule(&kernel, 5U) == ZZAPP_KERNEL_APP1);
	CHECK(kernel.threads[1].user_ticks == 5U);
	CHECK(kernel.overload_count == 1U);
}

/* Model the assembly order: entry, exit, another entry, then C consumes
 * the completed pair. */
#if ZZAPP_PERF_COUNTERS
static void test_performance_counters(void)
{
	zzapp_kernel_t kernel;
	zzapp_arm_frame_t frame = { 0 };
	zzapp_arm_frame_t *next;
	zzapp_kernel_perf_snapshot_t snapshot;
	zzapp_kernel_perf_metric_t overflow = { 0 };

	zzapp_kernel_init(&kernel);
	CHECK(zzapp_kernel_prepare_app(&kernel, 0, 0x1000u, 1u, 0u, 0u,
				       0x8000u) == 0);
	CHECK(zzapp_kernel_prepare_app(&kernel, 1, 0x2000u, 2u, 0u, 0u,
				       0x9000u) == 0);
	make_runnable(&kernel, 0);
	make_runnable(&kernel, 1);
	zzapp_kernel_set_current_thread(0);
	zzapp_kernel_switch_entry_stamp = 100u;
	zzapp_kernel_switch_exit_stamp = 160u;
	zzapp_kernel_switch_completed_entry_stamp = 100u;
	zzapp_kernel_switch_complete = 1u;
	zzapp_kernel_switch_entry_stamp = 200u;
	next = zzapp_kernel_switch_frame(&kernel, &frame,
					 ZZAPP_KERNEL_DISPATCH_TICK, 4u);
	zzapp_kernel_switch_exit_stamp = 280u;
	zzapp_kernel_switch_completed_entry_stamp = 200u;
	zzapp_kernel_switch_complete = 1u;
	zzapp_kernel_switch_entry_stamp = 300u;
	(void)zzapp_kernel_switch_frame(&kernel, next,
					ZZAPP_KERNEL_DISPATCH_TICK, 4u);
	zzapp_kernel_perf_sgi_raised(&kernel, 200u);
	zzapp_kernel_perf_sgi_started(&kernel, 260u);
	zzapp_kernel_perf_sgi_started(&kernel, 300u);
	zzapp_kernel_perf_sgi_raised(&kernel, 400u);
	zzapp_kernel_perf_sgi_started(&kernel, 460u);
	zzapp_kernel_perf_sdk_started(&kernel, 240u, 280u);
	zzapp_kernel_perf_service_begin(&kernel, ZZAPP_PERF_SERVICE_AUDIO, 300u);
	zzapp_kernel_perf_service_end(&kernel, ZZAPP_PERF_SERVICE_AUDIO, 380u);
	zzapp_kernel_perf_audio(&kernel, 1u, 3u, 4u, 2u);
	zzapp_kernel_perf_snapshot(&kernel, 1u, &snapshot);
	CHECK(snapshot.switch_cycles.samples == 2u);
	CHECK(snapshot.switch_cycles.max == 80u);
	CHECK(snapshot.sgi_to_service.samples == 2u);
	CHECK(snapshot.sgi_to_service.max == 60u);
	CHECK(snapshot.sdk_enqueue_to_start.samples == 1u);
	CHECK(snapshot.sdk_enqueue_to_start.max == 40u);
	CHECK(snapshot.service[ZZAPP_PERF_SERVICE_AUDIO].max == 80u);
	CHECK(snapshot.sgis == 2u);
	CHECK(snapshot.audio_low_water == 3u);
	CHECK(snapshot.audio_starved == 4u);
	CHECK(snapshot.audio_underruns == 2u);

	kernel.perf.report_pending = 0U;
	kernel.perf.report_force = 0U;

	zzapp_kernel_perf_request_report(&kernel, 1u, 0);
	CHECK((kernel.perf.report_pending & (1U << 1)) != 0U);
	CHECK(zzapp_kernel_perf_emit_reports(&kernel) == 1);
	CHECK(strstr(kernel.perf.report_text, "audio=3/4/2") != NULL);
	zzapp_kernel_perf_print_reports(&kernel);
	CHECK(kernel.perf.report_pending == 0U);
	zzapp_kernel_perf_request_report(&kernel, 1u, 0);
	CHECK(kernel.perf.report_pending == 0U);
	kernel.now += 10000u;
	zzapp_kernel_perf_request_report(&kernel, 1u, 0);
	zzapp_kernel_perf_request_report(&kernel, 2u, 0);
	CHECK(zzapp_kernel_perf_emit_reports(&kernel) == 1);
	CHECK((kernel.perf.report_pending & (1U << 2)) != 0U);
	zzapp_kernel_perf_print_reports(&kernel);
	CHECK(zzapp_kernel_perf_emit_reports(&kernel) == 0);
	zzapp_kernel_perf_request_report(&kernel, 3u, 1);
	CHECK(zzapp_kernel_perf_emit_reports(&kernel) == 1);
	CHECK(strstr(kernel.perf.report_text, "forced=1") != NULL);
	zzapp_kernel_perf_print_reports(&kernel);
	kernel.perf.audio_low_water[1] = 9u;
	kernel.perf.audio_starved[1] = 7u;
	kernel.perf.audio_underruns[1] = 8u;
	kernel.perf.tlb_instruction_refills[1] = 7u;
	kernel.perf.tlb_data_refills[1] = 6u;
	CHECK(zzapp_kernel_prepare_app(&kernel, 1, 0x3000u, 3u, 0u, 0u,
				       0xa000u) == 0);
	zzapp_kernel_perf_snapshot(&kernel, 1u, &snapshot);
	CHECK(snapshot.audio_low_water == 0u);
	CHECK(snapshot.audio_starved == 0u);
	CHECK(snapshot.audio_underruns == 0u);
	CHECK(snapshot.tlb_instruction_refills == 0u);
	CHECK(snapshot.tlb_data_refills == 0u);

	irq_save_calls = 0U;
	irq_restore_calls = 0U;
	zzapp_kernel_switch_completed_entry_stamp = 500u;
	zzapp_kernel_switch_exit_stamp = 550u;
	zzapp_kernel_switch_complete = 1u;
	zzapp_kernel_perf_switch_finished(&kernel);
	CHECK(irq_save_calls == 1U);
	CHECK(irq_restore_calls == 1U);

	irq_save_calls = 0U;
	irq_restore_calls = 0U;
	kernel.perf.tlb_slot = 0U;
	kernel.perf.tlb_instruction_stamp = 10U;
	kernel.perf.tlb_data_stamp = 20U;
	zzapp_kernel_perf_finish_slot(&kernel, 0U);
	CHECK(irq_save_calls == 1U);
	CHECK(irq_restore_calls == 1U);

	overflow.samples = 43383508u;
	overflow.histogram[0] = 1u;
	overflow.histogram[31] = overflow.samples - 1u;
	CHECK(zzapp_kernel_perf_test_percentile(&overflow, 99u) == (1U << 31));
}
#endif


int main(void)
{
    test_first_context_and_dispatch();
    test_frame_switch();
    test_system_frame_runs_service_before_apps();
    test_round_robin_quantum();
    test_ticks_through_system_thread_share_fairly();
    test_service_precedes_app_and_preserves_remainder();
    test_block_wake_and_idle();
    test_sleep_deadline_order();
    test_wait_completion_timeout_and_cancel();
    test_accounting_and_throttle();
    test_blocking_api_lock_and_slot_resources();
    test_waiter_isolation_timeout_and_stop();
    test_stop_wakes_api_lock_waiter();
    test_system_kernel_lock_handoffs();
    test_fault_releases_kernel_lock();
    test_kernel_stack_covers_decoder_call_chain();
    test_non_timer_interrupt_does_not_advance_time();
    test_system_ticks_wake_sleepers();
    test_restart_releases_kernel_lock_waiters();
    test_system_lock_inheritance_is_throttled();
#if ZZAPP_PERF_COUNTERS
    test_performance_counters();
#endif
    printf("kernel_sched_test: %u checks, %d failures\n", checks, failures);
    return failures != 0;
}
