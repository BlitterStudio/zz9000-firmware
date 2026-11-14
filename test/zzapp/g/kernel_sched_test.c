/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "zzapp_kernel.h"

static unsigned irq_save_calls;
static unsigned irq_restore_calls;
static uint64_t clock_ms;
static int test_irqs_masked;
static void (*test_pending_irq_fn)(void);
static void (*test_clock_hook)(void);

uint64_t zzapp_kernel_now_ms(void)
{
	if (test_clock_hook)
		test_clock_hook();
	return clock_ms;
}

uint32_t smp_local_irq_save(void)
{
	uint32_t prior = (uint32_t)test_irqs_masked;
	irq_save_calls++;
	test_irqs_masked = 1;
	return prior;
}

void smp_local_irq_restore(uint32_t state)
{
	irq_restore_calls++;
	test_irqs_masked = (int)state;
	if (!test_irqs_masked && test_pending_irq_fn) {
		void (*fn)(void) = test_pending_irq_fn;
		test_pending_irq_fn = NULL;
		fn();
	}
}

static void test_request_irq(void (*fn)(void))
{
	if (!test_irqs_masked) {
		fn();
	} else {
		test_pending_irq_fn = fn;
	}
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
	clock_ms = 0;
	next = zzapp_kernel_switch_frame(&kernel, &frame,
					 ZZAPP_KERNEL_DISPATCH_TICK, 4u);
	CHECK(next == &kernel.threads[0].context.frame);
	CHECK(kernel.threads[0].context.r[15] == 0x1000u);
	CHECK(kernel.threads[0].context.r[13] == 0x8000u);
	clock_ms = 5;
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
		clock_ms += 5;
		next = zzapp_kernel_switch_frame(&kernel, next,
						 ZZAPP_KERNEL_DISPATCH_TICK, 4u);
		if (zzapp_kernel_current_thread() == -1)
			next = zzapp_kernel_switch_frame(&kernel, next,
					ZZAPP_KERNEL_DISPATCH_VOLUNTARY, 0u);
	}
	CHECK(runs[0] + runs[1] == 200u);
	CHECK(runs[0] >= 95u && runs[1] >= 95u);
}

/* 4 ms of services then 1 ms of the app before the tick charges 4 and 1.
 * A voluntary handoff after 2 ms charges those 2 ms, not a full quantum. */
static void test_tick_charges_the_context_that_ran(void)
{
	zzapp_kernel_t kernel;
	zzapp_arm_frame_t frame = { 0 };
	zzapp_arm_frame_t *next;

	zzapp_kernel_init(&kernel);
	CHECK(zzapp_kernel_prepare_app(&kernel, 0, 0x1000u, 1u, 0u, 0u,
				       0x8000u) == 0);
	make_runnable(&kernel, 0);
	kernel.threads[0].state = ZZAPP_THREAD_BLOCKED;
	frame.spsr = ZZAPP_KERNEL_APP_CPSR;
	clock_ms = 0;
	zzapp_kernel_set_current_thread(-1);
	next = zzapp_kernel_switch_frame(&kernel, &frame,
					 ZZAPP_KERNEL_DISPATCH_VOLUNTARY, 0u);
	CHECK(zzapp_kernel_current_thread() == -1);
	kernel.threads[0].state = ZZAPP_THREAD_RUNNABLE;
	clock_ms = 4;
	next = zzapp_kernel_switch_frame(&kernel, next,
					 ZZAPP_KERNEL_DISPATCH_VOLUNTARY, 0u);
	CHECK(kernel.service_ticks == 4u);
	CHECK(kernel.threads[0].user_ticks == 0u);
	CHECK(zzapp_kernel_current_thread() == 0);
	clock_ms = 5;
	next->spsr = ZZAPP_KERNEL_APP_CPSR;
	next = zzapp_kernel_switch_frame(&kernel, next,
					 ZZAPP_KERNEL_DISPATCH_TICK, 4u);
	CHECK(kernel.threads[0].user_ticks == 1u);
	CHECK(kernel.service_ticks == 4u);
	CHECK(kernel.threads[0].svc_ticks == 0u);

	zzapp_kernel_init(&kernel);
	CHECK(zzapp_kernel_prepare_app(&kernel, 0, 0x1000u, 1u, 0u, 0u,
				       0x8000u) == 0);
	make_runnable(&kernel, 0);
	frame.spsr = ZZAPP_KERNEL_APP_CPSR;
	clock_ms = 10;
	zzapp_kernel_set_current_thread(0);
	next = zzapp_kernel_switch_frame(&kernel, &frame,
					 ZZAPP_KERNEL_DISPATCH_VOLUNTARY, 0u);
	clock_ms = 12;
	next = zzapp_kernel_switch_frame(&kernel, next,
					 ZZAPP_KERNEL_DISPATCH_VOLUNTARY, 0u);
	CHECK(kernel.threads[0].user_ticks == 2u);
	CHECK(kernel.service_ticks == 0u);
	zzapp_kernel_init(&kernel);
	CHECK(zzapp_kernel_prepare_app(&kernel, 0, 0x1000u, 1u, 0u, 0u,
				       0x8000u) == 0);
	make_runnable(&kernel, 0);
	frame.spsr = 0x1fu;
	clock_ms = 20;
	zzapp_kernel_set_current_thread(0);
	next = zzapp_kernel_switch_frame(&kernel, &frame,
					 ZZAPP_KERNEL_DISPATCH_VOLUNTARY, 0u);
	clock_ms = 23;
	next = zzapp_kernel_switch_frame(&kernel, &frame,
					 ZZAPP_KERNEL_DISPATCH_VOLUNTARY, 0u);
	CHECK(kernel.threads[0].svc_ticks == 3u);
	CHECK(kernel.threads[0].user_ticks == 0u);
	(void)next;
}

static void test_blocking_after_3ms_charges_3(void)
{
	zzapp_kernel_t kernel;
	zzapp_arm_frame_t frame = { 0 };
	zzapp_arm_frame_t *next;

	zzapp_kernel_init(&kernel);
	CHECK(zzapp_kernel_prepare_app(&kernel, 0, 0x1000u, 1u, 0u, 0u,
				       0x8000u) == 0);
	make_runnable(&kernel, 0);
	frame.spsr = ZZAPP_KERNEL_APP_CPSR;
	clock_ms = 0;
	zzapp_kernel_set_current_thread(-1);
	next = zzapp_kernel_switch_frame(&kernel, &frame,
					 ZZAPP_KERNEL_DISPATCH_VOLUNTARY, 0u);
	CHECK(zzapp_kernel_current_thread() == 0);
	clock_ms = 3;
	CHECK(zzapp_kernel_block(&kernel, 0) == 0);
	next = zzapp_kernel_switch_frame(&kernel, next,
					 ZZAPP_KERNEL_DISPATCH_VOLUNTARY, 0u);
	CHECK(kernel.threads[0].user_ticks == 3u);
	(void)next;
}

static void test_wfe_idle_gap_charges_nothing(void)
{
	zzapp_kernel_t kernel;
	zzapp_arm_frame_t frame = { 0 };
	zzapp_arm_frame_t *next;

	zzapp_kernel_init(&kernel);
	CHECK(zzapp_kernel_prepare_app(&kernel, 0, 0x1000u, 1u, 0u, 0u,
				       0x8000u) == 0);
	make_runnable(&kernel, 0);
	zzapp_kernel_set_current_thread(-1);
	clock_ms = 0;
	zzapp_kernel_idle_begin(&kernel);
	CHECK(kernel.idle == 1u);
	clock_ms = 50;
	zzapp_kernel_service_due(&kernel);
	next = zzapp_kernel_switch_frame(&kernel, &frame,
					 ZZAPP_KERNEL_DISPATCH_DOORBELL, 0u);
	CHECK(kernel.idle == 0u);
	CHECK(kernel.idle_ticks == 50u);
	CHECK(kernel.service_ticks == 0u);
	CHECK(kernel.service_in_window == 0u);
	CHECK(kernel.overload_count == 0u);
	CHECK(zzapp_kernel_current_thread() == -1);
	(void)next;
}

static void test_recovery_gap_charges_nothing(void)
{
	zzapp_kernel_t kernel;
	zzapp_arm_frame_t frame = { 0 };
	zzapp_arm_frame_t *next;

	zzapp_kernel_init(&kernel);
	zzapp_kernel_set_current_thread(-1);
	clock_ms = 10;
	next = zzapp_kernel_switch_frame(&kernel, &frame,
					 ZZAPP_KERNEL_DISPATCH_VOLUNTARY, 0u);
	clock_ms = 15;
	zzapp_kernel_reset_slice(&kernel);
	CHECK(kernel.slice_valid == 0u);
	clock_ms = 215;
	next = zzapp_kernel_switch_frame(&kernel, next,
					 ZZAPP_KERNEL_DISPATCH_VOLUNTARY, 0u);
	CHECK(kernel.service_ticks == 0u);
	CHECK(kernel.service_in_window == 0u);
	(void)next;
}

static zzapp_kernel_t *g_test_kernel;
static zzapp_arm_frame_t g_test_irq_frame;
static int g_test_irq_fired;
static int g_test_state_at_irq;
static uint64_t g_test_deadline_at_irq;

static void simulate_timer_irq(void)
{
	g_test_irq_fired++;
	g_test_state_at_irq = (int)g_test_kernel->threads[0].state;
	g_test_deadline_at_irq = g_test_kernel->threads[0].sleep_deadline;
	(void)zzapp_kernel_switch_frame(g_test_kernel, &g_test_irq_frame,
					ZZAPP_KERNEL_DISPATCH_TICK, 4u);
}

static void hook_trigger_sleep_irq(void)
{
	test_clock_hook = NULL;
	test_request_irq(simulate_timer_irq);
}

static void test_api_sleep_atomic_against_injected_irq(void)
{
	zzapp_kernel_t kernel;
	zzapp_arm_frame_t frame = { 0 };
	zzapp_arm_frame_t *next;

	zzapp_kernel_init(&kernel);
	CHECK(zzapp_kernel_prepare_app(&kernel, 0, 0x1000u, 1u, 0u, 0u,
				       0x8000u) == 0);
	make_runnable(&kernel, 0);
	CHECK(zzapp_kernel_api_lock(&kernel, 0) == 0);
	clock_ms = 0;
	zzapp_kernel_set_current_thread(-1);
	next = zzapp_kernel_switch_frame(&kernel, &frame,
					 ZZAPP_KERNEL_DISPATCH_VOLUNTARY, 0u);
	CHECK(zzapp_kernel_current_thread() == 0);

	/* Arm the clock hook: inside zzapp_kernel_api_sleep(), the clock lookup
	 * occurs while IRQs are masked. The hook requests an IRQ. Because IRQs
	 * are masked, the IRQ is pended and does not fire until unmask after sleep
	 * deadline publication. */
	g_test_kernel = &kernel;
	g_test_irq_frame = *next;
	g_test_irq_fired = 0;
	g_test_state_at_irq = -1;
	g_test_deadline_at_irq = 0U;
	test_clock_hook = hook_trigger_sleep_irq;

	CHECK(zzapp_kernel_api_sleep(&kernel, 0, 10u) == 0);
	/* Unmask delivered the pended IRQ, which ran switch_frame. */
	CHECK(g_test_irq_fired == 1);
	CHECK(g_test_state_at_irq == ZZAPP_THREAD_SLEEPING);
	CHECK(g_test_deadline_at_irq == 10u);
	CHECK(kernel.api_lock_owner == -1);
	CHECK(kernel.threads[0].state == ZZAPP_THREAD_SLEEPING);
	CHECK(kernel.threads[0].sleep_deadline == 10u);

	/* 10 ms elapse: the deadline arrives and the sleeper wakes up. */
	clock_ms = 10;
	zzapp_kernel_advance(&kernel, 10u);
	CHECK(kernel.threads[0].state == ZZAPP_THREAD_RUNNABLE);
}

static void simulate_idle_timer_irq(void)
{
	g_test_irq_fired++;
	clock_ms = 20;
	(void)zzapp_kernel_switch_frame(g_test_kernel, &g_test_irq_frame,
					ZZAPP_KERNEL_DISPATCH_TICK, 4u);
}

static void hook_trigger_idle_irq(void)
{
	test_clock_hook = NULL;
	test_request_irq(simulate_idle_timer_irq);
}

static void test_idle_irq_no_double_charge(void)
{
	zzapp_kernel_t kernel;
	zzapp_arm_frame_t frame = { 0 };

	zzapp_kernel_init(&kernel);
	zzapp_kernel_set_current_thread(-1);
	clock_ms = 0;

	/* Arm the clock hook: inside zzapp_kernel_idle_begin(), zzapp_kernel_now_ms()
	 * runs while IRQs are masked. The hook requests an IRQ.
	 * Because IRQs are masked, idle_begin sets idle = 1 before the IRQ fires
	 * at unmask. Switch_frame sees was_idle = 1, charges idle_ticks, and
	 * does not charge service_ticks. */
	g_test_kernel = &kernel;
	g_test_irq_frame = frame;
	g_test_irq_fired = 0;
	test_clock_hook = hook_trigger_idle_irq;

	zzapp_kernel_idle_begin(&kernel);
	/* smp_local_irq_restore at the end of idle_begin delivered the IRQ. */
	CHECK(g_test_irq_fired == 1);
	CHECK(kernel.idle == 0u);
	CHECK(kernel.idle_ticks == 20u);
	CHECK(kernel.service_ticks == 0u);
	CHECK(kernel.service_in_window == 0u);

	/* Returning to core 1 loop after IRQ executes idle_end at t = 25 ms.
	 * Since idle is 0, idle_end does not double-charge. */
	clock_ms = 25;
	zzapp_kernel_idle_end(&kernel);
	CHECK(kernel.idle == 0u);
	CHECK(kernel.idle_ticks == 20u);
	CHECK(kernel.service_ticks == 0u);
	CHECK(kernel.service_in_window == 0u);
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

static void test_new_generation_resets_cpu_ticks(void)
{
	zzapp_kernel_t kernel;

	zzapp_kernel_init(&kernel);
	kernel.threads[0].user_ticks = 17U;
	kernel.threads[0].svc_ticks = 23U;
	zzapp_kernel_reset_cpu_ticks(&kernel, 0U);
	CHECK(kernel.threads[0].user_ticks == 0U);
	CHECK(kernel.threads[0].svc_ticks == 0U);
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
	clock_ms = 0;
	next = zzapp_kernel_switch_frame(&kernel, &system_frame,
					 ZZAPP_KERNEL_DISPATCH_VOLUNTARY, 0u);
	clock_ms = 5;
	next = zzapp_kernel_switch_frame(&kernel, next,
					 ZZAPP_KERNEL_DISPATCH_TICK, 4u);
	CHECK(next == &kernel.system_context.frame);
	CHECK(kernel.now == 5u);
	CHECK(kernel.threads[0].state == ZZAPP_THREAD_SLEEPING);
	clock_ms = 10;
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
    test_tick_charges_the_context_that_ran();
    test_blocking_after_3ms_charges_3();
    test_wfe_idle_gap_charges_nothing();
    test_recovery_gap_charges_nothing();
    test_api_sleep_atomic_against_injected_irq();
    test_idle_irq_no_double_charge();
    test_service_precedes_app_and_preserves_remainder();
    test_block_wake_and_idle();
    test_sleep_deadline_order();
    test_wait_completion_timeout_and_cancel();
    test_accounting_and_throttle();
    test_new_generation_resets_cpu_ticks();
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
